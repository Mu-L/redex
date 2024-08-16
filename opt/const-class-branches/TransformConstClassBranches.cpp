/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TransformConstClassBranches.h"

#include <algorithm>
#include <mutex>

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "Creators.h"
#include "DexClass.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "SourceBlocks.h"
#include "StringTreeSet.h"
#include "SwitchEquivFinder.h"
#include "SwitchEquivPrerequisites.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_METHODS_TRANSFORMED = "num_methods_transformed";
constexpr const char* METRIC_CONST_CLASS_INSTRUCTIONS_REMOVED =
    "num_const_class_instructions_removed";
constexpr const char* METRIC_TOTAL_STRING_SIZE = "total_string_size";

// Holder for the pass's configuration options.
struct PassState {
  DexMethodRef* lookup_method;
  bool consider_external_classes;
  size_t min_cases;
  size_t max_cases;
  std::mutex& transforms_mutex;
};

// Denotes a branch and successor blocks within a method that can be
// successfully represented/transformed.
struct BranchTransform {
  cfg::Block* block;
  IRInstruction* insn;
  reg_t determining_reg;
  std::unique_ptr<SwitchEquivFinder> switch_equiv;
};

// Denotes a method that will have one or many transforms.
struct MethodTransform {
  DexMethod* method;
  std::unique_ptr<IRCode> code_copy;
  std::unique_ptr<cfg::ScopedCFG> scoped_cfg;
  std::vector<BranchTransform> transforms;
};

struct Stats {
  size_t methods_transformed{0};
  size_t const_class_instructions_removed{0};
  size_t string_tree_size{0};
  Stats& operator+=(const Stats& that) {
    methods_transformed += that.methods_transformed;
    const_class_instructions_removed += that.const_class_instructions_removed;
    string_tree_size += that.string_tree_size;
    return *this;
  }
};

size_t num_const_class_opcodes(const cfg::ControlFlowGraph* cfg) {
  size_t result{0};
  for (auto& mie : InstructionIterable(*cfg)) {
    if (mie.insn->opcode() == OPCODE_CONST_CLASS) {
      result++;
    }
  }
  return result;
}

// This pass cares about comparing objects, so only eq, ne are relevant at the
// end of a block.
bool ends_in_if_statment(const cfg::Block* b) {
  auto it = b->get_last_insn();
  if (it == b->end()) {
    return false;
  }
  auto op = it->insn->opcode();
  return opcode::is_if_eq(op) || opcode::is_if_ne(op);
}

// Meant to be a quick guess, to skip some of the preliminary work in deciding
// for real if the method should be operated upon if nothing looks relevant.
bool should_consider_method(const PassState& pass_state, DexMethod* method) {
  if (method->rstate.no_optimizations()) {
    return false;
  }
  auto code = method->get_code();
  if (code == nullptr) {
    return false;
  }
  auto& cfg = code->cfg();
  bool found_branch = false;
  for (const auto* b : cfg.blocks()) {
    // Note: SwitchEquivFinder assumes the non-leaf blocks (the blocks that
    // perform equals checks) have no throw edges. Avoid considering such a
    // method early on.
    if (b->is_catch()) {
      return false;
    }
    if (ends_in_if_statment(b)) {
      found_branch = true;
      break;
    }
  }
  return found_branch && num_const_class_opcodes(&cfg) >= pass_state.min_cases;
}

// True if the finder is successful, has a default block and does not have some
// edge cases we don't wanna deal with right now.
bool finder_results_are_supported(SwitchEquivFinder* finder) {
  return finder->success() &&
         finder->are_keys_uniform(SwitchEquivFinder::KeyKind::CLASS) &&
         finder->extra_loads().empty() && finder->default_case();
}

// Rather than looping over the cfg blocks, explicitly start from the entry
// block and bfs through the graph. Makes sure that even if the cfg got
// manipulated such that entry block is not the smallest id, we will start
// looking for eligible transforms roughly from that point.
void order_blocks(const cfg::ControlFlowGraph& cfg,
                  std::vector<cfg::Block*>* out) {
  std::stack<cfg::Block*> to_visit;
  std::unordered_set<cfg::BlockId> visited;
  to_visit.push(cfg.entry_block());
  while (!to_visit.empty()) {
    cfg::Block* b = to_visit.top();
    to_visit.pop();

    if (visited.count(b->id()) > 0) {
      continue;
    }
    visited.emplace(b->id());
    out->emplace_back(b);
    for (cfg::Edge* e : b->succs()) {
      to_visit.push(e->target());
    }
  }
}

namespace cp = constant_propagation;

void gather_possible_transformations(
    const PassState& pass_state,
    DexMethod* method,
    std::vector<MethodTransform>* method_transforms) {
  // First step is to operate on a simplified copy of the code. If the transform
  // is applicable, this copy will take effect.
  auto code_copy = std::make_unique<IRCode>(*method->get_code());
  SwitchEquivEditor::simplify_moves(code_copy.get());
  auto scoped_cfg = std::make_unique<cfg::ScopedCFG>(code_copy.get());
  auto& cfg = **scoped_cfg;

  std::vector<BranchTransform> transforms;

  TRACE(CCB, 3, "Checking for const-class branching in %s", SHOW(method));
  auto fixpoint = std::make_shared<cp::intraprocedural::FixpointIterator>(
      /* cp_state */ nullptr, cfg, SwitchEquivFinder::Analyzer());
  fixpoint->run(ConstantEnvironment());

  std::vector<cfg::Block*> blocks;
  order_blocks(cfg, &blocks);
  std::unordered_set<cfg::Block*> blocks_considered;
  for (const auto& b : blocks) {
    if (blocks_considered.count(b) > 0) {
      continue;
    }
    blocks_considered.emplace(b);
    reg_t determining_reg;
    if (ends_in_if_statment(b) &&
        find_determining_reg(*fixpoint, b, &determining_reg)) {
      // Keep going, maybe this block is a useful starting point.
      TRACE(CCB, 2, "determining_reg is %d for B%zu", determining_reg, b->id());
      auto last_insn = b->get_last_insn()->insn;
      auto root_branch = cfg.find_insn(last_insn);
      auto finder = std::make_unique<SwitchEquivFinder>(
          &cfg, root_branch, determining_reg,
          SwitchEquivFinder::NO_LEAF_DUPLICATION, fixpoint,
          SwitchEquivFinder::EXECUTION_ORDER);
      if (finder_results_are_supported(finder.get())) {
        TRACE(CCB, 2, "SwitchEquivFinder succeeded on B%zu for branch at: %s",
              b->id(), SHOW(last_insn));
        auto visited = finder->visited_blocks();
        std::copy(visited.begin(), visited.end(),
                  std::inserter(blocks_considered, blocks_considered.end()));
        const auto& key_to_case = finder->key_to_case();
        size_t relevant_case_count{0};
        for (auto&& [key, leaf] : key_to_case) {
          if (!SwitchEquivFinder::is_default_case(key)) {
            auto dtype = boost::get<const DexType*>(key);
            auto case_class = type_class(dtype);
            if (pass_state.consider_external_classes ||
                (case_class != nullptr && !case_class->is_external())) {
              relevant_case_count++;
            }
          }
        }
        if (relevant_case_count > pass_state.max_cases ||
            relevant_case_count < pass_state.min_cases) {
          TRACE(CCB, 2, "Not considering branch due to number of cases.");
          continue;
        }
        // Part of this method should conform to expectations, note this.
        BranchTransform transform{b, last_insn, determining_reg,
                                  std::move(finder)};
        transforms.emplace_back(std::move(transform));
      }
    }
  }
  if (!transforms.empty()) {
    std::lock_guard<std::mutex> lock(pass_state.transforms_mutex);
    MethodTransform mt{method, std::move(code_copy), std::move(scoped_cfg),
                       std::move(transforms)};
    method_transforms->emplace_back(std::move(mt));
  }
}

Stats apply_transform(const PassState& pass_state,
                      MethodTransform& mt,
                      size_t transform_count) {
  Stats result;
  auto method = mt.method;
  auto cls = type_class(method->get_class());

  // Creates a method on the class that returns the given encoded string; broken
  // out into a separate method in an attempt to work around suspected JIT bug
  // on Android Go devices. This truly makes no sense, but here we are.
  auto int_arg = DexTypeList::make_type_list({type::_int()});
  DexProto* string_getter_proto =
      DexProto::make_proto(type::java_lang_String(), int_arg);
  /**
   * Created method below is inspired from:
   *
   * String myConstImpl(int depth) {
   *   if (depth >= 10) {
   *     throw new RuntimeError("WTF");
   *   }
   *   String s = "...";
   *   if (s == null) {
   *     return myConstImpl(depth + 1);
   *   }
   *   return s;
   * }
   */
  auto create_string_getter_method = [&](const DexString* encoded_str) {
    MethodCreator creator(
        method->get_class(),
        DexString::make_string("__RDX_GET_STR_" +
                               std::to_string(transform_count)),
        string_getter_proto, ACC_STATIC | ACC_PRIVATE);

    auto getter = creator.create();
    getter->rstate.set_no_optimizations();
    getter->rstate.set_generated();
    cls->add_method(getter);

    auto code = getter->get_code();
    code->build_cfg();
    auto& getter_cfg = code->cfg();

    auto entry = getter_cfg.entry_block();
    // -> branch to either:
    auto throw_block = getter_cfg.create_block();
    auto non_throw_block = getter_cfg.create_block();
    // -> branch to either:
    auto recurse_block = getter_cfg.create_block();
    auto non_null_block = getter_cfg.create_block();

    // Main blocks that checks depth
    auto max_depth_reg = getter_cfg.allocate_temp();
    auto const_max = new IRInstruction(OPCODE_CONST);
    const_max->set_literal(10);
    const_max->set_dest(max_depth_reg);
    entry->push_back(const_max);
    getter_cfg.create_branch(entry,
                             (new IRInstruction(OPCODE_IF_GE))
                                 ->set_src(0, 0)
                                 ->set_src(1, max_depth_reg),
                             non_throw_block, throw_block);

    // throwing block
    auto ex_reg = getter_cfg.allocate_temp();
    auto msg_reg = getter_cfg.allocate_temp();
    auto new_instance =
        (new IRInstruction(OPCODE_NEW_INSTANCE))
            ->set_type(DexType::get_type("Ljava/lang/RuntimeException;"));
    auto move_ex = (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                       ->set_dest(ex_reg);
    auto msg = (new IRInstruction(OPCODE_CONST_STRING))
                   ->set_string(DexString::make_string("Unexpected"));
    auto move_msg = (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                        ->set_dest(msg_reg);
    auto invoke_init =
        (new IRInstruction(OPCODE_INVOKE_DIRECT))
            ->set_srcs_size(2)
            ->set_src(0, ex_reg)
            ->set_src(1, msg_reg)
            ->set_method(DexMethod::get_method(
                "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V"));
    auto throw_ex = (new IRInstruction(OPCODE_THROW))->set_src(0, ex_reg);
    throw_block->push_back(
        {new_instance, move_ex, msg, move_msg, invoke_init, throw_ex});

    // non-throwing block
    auto str_reg = getter_cfg.allocate_temp();
    auto str =
        (new IRInstruction(OPCODE_CONST_STRING))->set_string(encoded_str);
    auto move_str = (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                        ->set_dest(str_reg);
    non_throw_block->push_back({str, move_str});
    getter_cfg.create_branch(
        non_throw_block,
        (new IRInstruction(OPCODE_IF_NEZ))->set_src(0, str_reg), recurse_block,
        non_null_block);

    // return the string
    auto ret = (new IRInstruction(OPCODE_RETURN_OBJECT))->set_src(0, str_reg);
    non_null_block->push_back(ret);

    // increment and recurse
    auto inc_reg = getter_cfg.allocate_temp();
    auto recurse_result_reg = getter_cfg.allocate_temp();
    auto add_lit = (new IRInstruction(OPCODE_ADD_INT_LIT))
                       ->set_literal(1)
                       ->set_src(0, 0)
                       ->set_dest(inc_reg);
    auto recurse = (new IRInstruction(OPCODE_INVOKE_STATIC))
                       ->set_srcs_size(1)
                       ->set_method(getter)
                       ->set_src(0, inc_reg);
    auto move_recurse = (new IRInstruction(OPCODE_MOVE_RESULT_OBJECT))
                            ->set_dest(recurse_result_reg);
    auto ret_recurse = (new IRInstruction(OPCODE_RETURN_OBJECT))
                           ->set_src(0, recurse_result_reg);
    recurse_block->push_back({add_lit, recurse, move_recurse, ret_recurse});

    TRACE(CCB, 4, "String getter method %s %s", SHOW(getter), SHOW(getter_cfg));
    return getter;
  };

  auto& cfg = **mt.scoped_cfg;
  auto before_const_class_count = num_const_class_opcodes(&cfg);
  TRACE(CCB, 3,
        "Processing const-class branching in %s (transform size = %zu) %s",
        SHOW(method), mt.transforms.size(), SHOW(cfg));

  for (auto& transform : mt.transforms) {
    // Determine stable order of the types that are being switched on.
    std::set<const DexType*, dextypes_comparator> ordered_types;
    const auto& key_to_case = transform.switch_equiv->key_to_case();
    cfg::Block* default_case{nullptr};
    for (auto&& [key, block] : key_to_case) {
      if (!SwitchEquivFinder::is_default_case(key)) {
        auto dtype = boost::get<const DexType*>(key);
        ordered_types.emplace(dtype);
      } else {
        TRACE(CCB, 3, "DEFAULT -> B%zu\n%s", block->id(), SHOW(block));
        default_case = block;
      }
    }
    // Create ordinals for each type being switched on, reserving zero to denote
    // an explicit default case.
    std::map<std::string, int16_t> string_tree_items;
    std::vector<std::pair<int32_t, cfg::Block*>> new_edges;
    constexpr int16_t STRING_TREE_NO_ENTRY = 0;
    int16_t counter = STRING_TREE_NO_ENTRY + 1;
    for (const auto& type : ordered_types) {
      auto string_name = java_names::internal_to_external(type->str_copy());
      int16_t ordinal = counter++;
      string_tree_items.emplace(string_name, ordinal);
      auto block = key_to_case.at(type);
      new_edges.emplace_back(ordinal, block);
      TRACE(CCB, 3, "%s (%s) -> B%zu\n%s", SHOW(type), string_name.c_str(),
            block->id(), SHOW(block));
    }

    auto encoded_str =
        StringTreeMap<int16_t>::encode_string_tree_map(string_tree_items);
    result.string_tree_size = encoded_str.size();
    auto encoded_dex_str = DexString::make_string(encoded_str);

    // Fiddle with the block's last instruction and install an actual switch
    TRACE(CCB, 2, "Removing B%zu's last instruction: %s", transform.block->id(),
          SHOW(transform.insn));

    std::vector<IRInstruction*> replacements;
    auto zero_depth_reg = cfg.allocate_temp();
    auto zero_insn = new IRInstruction(OPCODE_CONST);
    zero_insn->set_literal(0);
    zero_insn->set_dest(zero_depth_reg);
    replacements.push_back(zero_insn);
    auto encoded_str_reg = cfg.allocate_temp();

    auto getter_ref = create_string_getter_method(encoded_dex_str);
    auto get_string_insn =
        (new IRInstruction(OPCODE_INVOKE_STATIC))->set_method(getter_ref);
    get_string_insn->set_srcs_size(1);
    get_string_insn->set_src(0, zero_depth_reg);
    replacements.push_back(get_string_insn);
    auto move_string_insn = (new IRInstruction(OPCODE_MOVE_RESULT_OBJECT))
                                ->set_dest(encoded_str_reg);
    replacements.push_back(move_string_insn);

    auto default_value_reg = cfg.allocate_temp();
    auto default_value_const = new IRInstruction(OPCODE_CONST);
    default_value_const->set_literal(STRING_TREE_NO_ENTRY);
    default_value_const->set_dest(default_value_reg);
    replacements.push_back(default_value_const);

    auto invoke_string_tree = new IRInstruction(OPCODE_INVOKE_STATIC);
    invoke_string_tree->set_method(pass_state.lookup_method);
    invoke_string_tree->set_srcs_size(3);
    invoke_string_tree->set_src(0, transform.determining_reg);
    invoke_string_tree->set_src(1, encoded_str_reg);
    invoke_string_tree->set_src(2, default_value_reg);
    replacements.push_back(invoke_string_tree);

    // Just reuse a reg we don't need anymore
    auto switch_result_reg = default_value_reg;
    auto move_lookup_result = new IRInstruction(OPCODE_MOVE_RESULT);
    move_lookup_result->set_dest(switch_result_reg);
    replacements.push_back(move_lookup_result);

    auto new_switch = new IRInstruction(OPCODE_SWITCH);
    new_switch->set_src(0, switch_result_reg);
    // Note: it seems instruction "new_switch" gets appended via create_branch;
    // no need to push to replacements

    cfg.replace_insns(cfg.find_insn(transform.insn), replacements);
    // We are explicitly covering the default block via the default return value
    // from the string tree. Not needed here.
    cfg.create_branch(transform.block, new_switch, nullptr, new_edges);

    // Reset successor of last prologue block to implement the default case.
    for (auto& edge : transform.block->succs()) {
      if (edge->type() == cfg::EDGE_GOTO) {
        cfg.set_edge_target(edge, default_case);
      }
    }
    transform_count++;
  }
  // Last step is to prune leaf blocks which are now unreachable. Do this
  // before computing metrics (so we know if this pass is doing anything
  // useful) but be sure to not dereference any Block ptrs from here on out!
  cfg.remove_unreachable_blocks();
  TRACE(CCB, 3, "POST EDIT %s", SHOW(cfg));
  result.methods_transformed = 1;
  // Metric is not entirely accurate as we don't do dce on the first block that
  // starts the if chain (eehhh close enough).
  result.const_class_instructions_removed =
      before_const_class_count - num_const_class_opcodes(&cfg);
  always_assert(result.const_class_instructions_removed >= 0);

  // Make the copy take effect.
  method->set_code(std::move(mt.code_copy));
  return result;
}

} // namespace

void TransformConstClassBranchesPass::bind_config() {
  bind("consider_external_classes", false, m_consider_external_classes);
  // Probably not worthwhile for tiny methods.
  bind("min_cases", 5, m_min_cases);
  // Arbitrary default values to avoid creating unbounded amounts of encoded
  // string data.
  bind("max_cases", 2000, m_max_cases);
  bind("string_tree_lookup_method", "", m_string_tree_lookup_method);
  // Applying runtime workarounds per string generated, at the moment, will
  // involve generating extra helper methods. Put some sensible cap on number of
  // transforms to give the ability to reserve refs
  bind("transforms_per_dex", 10, m_max_transforms_per_dex);
  trait(Traits::Pass::unique, true);
}

void TransformConstClassBranchesPass::eval_pass(DexStoresVector&,
                                                ConfigFiles&,
                                                PassManager& mgr) {
  // Every transform will get a method that returns the generated string, that
  // method will itself call constructor of RuntimeException under weird
  // situations, and 1 more ref for the actual call to the lookup method.
  auto mrefs = 2 + m_max_transforms_per_dex;
  m_reserved_refs_handle = mgr.reserve_refs(name(),
                                            ReserveRefsInfo(/* frefs */ 0,
                                                            /* trefs */ 1,
                                                            mrefs));
}

void TransformConstClassBranchesPass::run_pass(DexStoresVector& stores,
                                               ConfigFiles& /* unused */,
                                               PassManager& mgr) {
  always_assert(m_reserved_refs_handle);
  mgr.release_reserved_refs(*m_reserved_refs_handle);
  m_reserved_refs_handle = std::nullopt;

  auto scope = build_class_scope(stores);
  if (m_string_tree_lookup_method.empty()) {
    TRACE(CCB, 1, "Pass not configured; returning.");
    return;
  }
  auto string_tree_lookup_method =
      DexMethod::get_method(m_string_tree_lookup_method);
  if (string_tree_lookup_method == nullptr) {
    TRACE(CCB, 1, "Lookup method not found; returning.");
    return;
  }

  std::vector<MethodTransform> method_transforms;
  std::mutex transforms_mutex;
  PassState pass_state{string_tree_lookup_method, m_consider_external_classes,
                       m_min_cases, m_max_cases, transforms_mutex};
  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (should_consider_method(pass_state, method)) {
      gather_possible_transformations(pass_state, method, &method_transforms);
    }
  });

  std::unordered_map<DexClass*, std::vector<MethodTransform*>>
      per_class_transforms;
  for (auto& transform : method_transforms) {
    auto cls = type_class(transform.method->get_class());
    per_class_transforms[cls].emplace_back(&transform);
  }

  Stats stats;
  // Apply at most N transforms per dex, because of reserved refs.
  auto apply_transforms_dex = [&](DexClasses& dex_file) {
    std::vector<MethodTransform*> per_dex_transforms;
    for (auto cls : dex_file) {
      auto search = per_class_transforms.find(cls);
      if (search != per_class_transforms.end()) {
        per_dex_transforms.insert(per_dex_transforms.end(),
                                  search->second.begin(), search->second.end());
      }
    }
    std::sort(per_dex_transforms.begin(), per_dex_transforms.end(),
              [](const MethodTransform* a, const MethodTransform* b) {
                return compare_dexmethods(a->method, b->method);
              });
    size_t transform_count{0};
    for (auto it = per_dex_transforms.rbegin(); it != per_dex_transforms.rend();
         ++it) {
      auto& mt = *it;
      auto size = mt->transforms.size();
      if (transform_count + size > m_max_transforms_per_dex) {
        break;
      }
      stats += apply_transform(pass_state, *mt, transform_count);
      transform_count += size;
    }
  };

  for (auto& store : stores) {
    auto& dex_files = store.get_dexen();
    for (auto& dex_file : dex_files) {
      apply_transforms_dex(dex_file);
    }
  }
  mgr.incr_metric(METRIC_METHODS_TRANSFORMED, stats.methods_transformed);
  mgr.incr_metric(METRIC_CONST_CLASS_INSTRUCTIONS_REMOVED,
                  stats.const_class_instructions_removed);
  mgr.incr_metric(METRIC_TOTAL_STRING_SIZE, stats.string_tree_size);
  TRACE(CCB, 1,
        "[transform const-class branches] Altered %zu method(s) to remove %zu "
        "const-class instructions; %zu bytes of character data created.",
        stats.methods_transformed, stats.const_class_instructions_removed,
        stats.string_tree_size);
}

static TransformConstClassBranchesPass s_pass;
