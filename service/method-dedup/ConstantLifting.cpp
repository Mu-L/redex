/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantLifting.h"

#include "AnnoUtils.h"
#include "ConstantValue.h"
#include "ControlFlow.h"
#include "IRCode.h"
#include "MethodReference.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeReference.h"
#include "TypeTags.h"

namespace {

constexpr const char* METHOD_META =
    "Lcom/facebook/redex/annotations/MethodMeta;";
constexpr const char* CONST_TYPE_ANNO_ATTR_NAME = "constantTypes";
constexpr const char* CONST_VALUE_ANNO_ATTR_NAME = "constantValues";

bool overlaps_with_an_existing_virtual_scope(DexType* type,
                                             const DexString* name,
                                             DexProto* proto) {
  if (DexMethod::get_method(type, name, proto)) {
    return true;
  }
  DexClass* cls = type_class(type);
  while (cls->get_super_class()) {
    type = cls->get_super_class();
    if (DexMethod::get_method(type, name, proto)) {
      return true;
    }
    cls = type_class(type);
  }

  return false;
}

} // namespace

const DexType* s_method_meta_anno;

ConstantLifting::ConstantLifting() : m_num_const_lifted_methods(0) {
  s_method_meta_anno = DexType::get_type(DexString::get_string(METHOD_META));
}

bool ConstantLifting::is_applicable_to_constant_lifting(
    const DexMethod* method) {
  if (is_synthetic(method) || !has_anno(method, s_method_meta_anno)) {
    return false;
  }
  if (!has_attribute(method, s_method_meta_anno, CONST_TYPE_ANNO_ATTR_NAME)) {
    return false;
  }
  return true;
}

std::vector<DexMethod*> ConstantLifting::lift_constants_from(
    const Scope& scope,
    const TypeTags* type_tags,
    const std::vector<DexMethod*>& methods,
    const size_t stud_method_threshold) {
  std::unordered_set<DexMethod*> lifted;
  std::unordered_map<DexMethod*, ConstantValues> lifted_constants;
  for (auto method : methods) {
    always_assert(has_anno(method, s_method_meta_anno));
    auto kinds_str = parse_str_anno_value(method, s_method_meta_anno,
                                          CONST_TYPE_ANNO_ATTR_NAME);
    auto vals_str = parse_str_anno_value(method, s_method_meta_anno,
                                         CONST_VALUE_ANNO_ATTR_NAME);

    always_assert(method->get_code()->editable_cfg_built());
    auto& cfg = method->get_code()->cfg();
    ConstantValues const_vals(type_tags, kinds_str, vals_str,
                              stud_method_threshold, cfg);
    auto const_loads = const_vals.collect_constant_loads(cfg);
    if (const_loads.empty()) {
      // No matching constant found.
      TRACE(METH_DEDUP,
            5,
            "  no matching constant %s found in %s",
            const_vals.to_str().c_str(),
            SHOW(method));
      TRACE(METH_DEDUP, 9, "%s", SHOW(cfg));
      continue;
    }

    TRACE(METH_DEDUP,
          5,
          "constant lifting: const value %s",
          const_vals.to_str().c_str());
    TRACE(METH_DEDUP, 9, "    in %s", SHOW(method));
    TRACE(METH_DEDUP, 9, "%s", SHOW(cfg));

    // Add constant to arg list.
    auto old_proto = method->get_proto();
    auto const_types = const_vals.get_constant_types();
    auto arg_list = old_proto->get_args()->push_back(const_types);
    auto new_proto = DexProto::make_proto(old_proto->get_rtype(), arg_list);

    // Find a non-conflicting name
    auto name = method->get_name();
    std::string suffix = "$r";
    while (overlaps_with_an_existing_virtual_scope(method->get_class(), name,
                                                   new_proto)) {
      name = DexString::make_string(name->c_str() + suffix);
      TRACE(METH_DEDUP,
            9,
            "constant lifting method name updated to %s",
            name->c_str());
    }

    // Update method
    DexMethodSpec spec;
    spec.name = name;
    spec.proto = new_proto;
    method->change(spec, true /* rename on collision */);

    // Insert param load.
    auto block = cfg.entry_block();
    auto last_loading = block->get_last_param_loading_insn();
    for (const auto& const_val : const_vals.get_constant_values()) {
      if (const_val.is_invalid()) {
        continue;
      }
      auto load_type_tag_param =
          const_val.is_int_value()
              ? new IRInstruction(IOPCODE_LOAD_PARAM)
              : new IRInstruction(IOPCODE_LOAD_PARAM_OBJECT);
      load_type_tag_param->set_dest(const_val.get_param_reg());
      if (last_loading != block->end()) {
        cfg.insert_after(block->to_cfg_instruction_iterator(last_loading),
                         load_type_tag_param);
      } else {
        cfg.insert_before(block->to_cfg_instruction_iterator(
                              block->get_first_non_param_loading_insn()),
                          load_type_tag_param);
      }
      last_loading = block->get_last_param_loading_insn();
    }

    // Replace const loads with moves.
    for (const auto& load : const_loads) {
      auto const_val = load.first;
      auto insn_it = load.second.first;
      auto dest = load.second.second;
      auto move_const_arg = const_val.is_int_value()
                                ? new IRInstruction(OPCODE_MOVE)
                                : new IRInstruction(OPCODE_MOVE_OBJECT);
      move_const_arg->set_dest(dest);
      move_const_arg->set_src(0, const_val.get_param_reg());
      cfg.insert_before(insn_it, move_const_arg);
      cfg.remove_insn(insn_it);
    }

    lifted.insert(method);
    lifted_constants.emplace(method, const_vals);
    TRACE(METH_DEDUP, 9, "const value lifted in \n%s", SHOW(cfg));
  }
  TRACE(METH_DEDUP,
        5,
        "constant lifting applied to %zu among %zu",
        lifted.size(),
        methods.size());
  m_num_const_lifted_methods += lifted.size();

  // Update call sites
  std::vector<DexMethod*> stub_methods;
  auto call_sites = method_reference::collect_call_refs(scope, lifted);
  for (const auto& callsite : call_sites) {
    auto meth = callsite.caller;
    auto* insn = callsite.insn;
    const auto callee =
        resolve_method(insn->get_method(), opcode_to_search(insn));
    always_assert(callee != nullptr);
    auto const_vals = lifted_constants.at(callee);
    auto& meth_cfg = meth->get_code()->cfg();
    auto cfg_it = meth_cfg.find_insn(insn);
    if (const_vals.needs_stub()) {
      // Insert const load
      std::vector<reg_t> args;
      for (size_t i = 0; i < insn->srcs_size(); i++) {
        args.push_back(insn->src(i));
      }
      auto stub = const_vals.create_stub_method(callee);
      stub->get_code()->build_cfg();
      auto invoke = method_reference::make_invoke(stub, insn->opcode(), args);
      IRInstruction* orig_invoke = cfg_it->insn;
      cfg_it->insn = invoke;
      // remove original call.
      delete orig_invoke;
      stub_methods.push_back(stub);
    } else {
      // Make const load
      std::vector<reg_t> const_regs;
      for (size_t i = 0; i < const_vals.size(); ++i) {
        const_regs.push_back(meth_cfg.allocate_temp());
      }
      auto const_loads = const_vals.make_const_loads(const_regs);
      // Insert const load
      std::vector<reg_t> args;
      for (size_t i = 0; i < insn->srcs_size(); i++) {
        args.push_back(insn->src(i));
      }
      args.insert(args.end(), const_regs.begin(), const_regs.end());
      meth_cfg.insert_before(cfg_it, const_loads);
      auto orig_invoke = cfg_it->insn;
      auto invoke = method_reference::make_invoke(callee, insn->opcode(), args);
      // replace to the call.
      cfg_it->insn = invoke;
      // remove original call.
      delete orig_invoke;
    }
    TRACE(METH_DEDUP, 9, " patched call site in %s\n%s", SHOW(meth),
          SHOW(meth_cfg));
  }

  return stub_methods;
}
