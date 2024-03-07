/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GlobalTypeAnalysisPass.h"
#include "Pass.h"
#include "RemoveUnreachable.h"

class TypeAnalysisAwareRemoveUnreachablePass
    : public RemoveUnreachablePassBase {
 public:
  TypeAnalysisAwareRemoveUnreachablePass()
      : RemoveUnreachablePassBase("TypeAnalysisAwareRemoveUnreachablePass") {}

  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<GlobalTypeAnalysisPass>();
    au.set_preserve_all();
  }

  std::unique_ptr<reachability::ReachableObjects> compute_reachable_objects(
      const Scope& scope,
      const method_override_graph::Graph& method_override_graph,
      PassManager& pm,
      int* num_ignore_check_strings,
      reachability::ReachableAspects* reachable_aspects,
      bool emit_graph_this_run,
      bool relaxed_keep_class_members,
      bool relaxed_keep_interfaces,
      bool cfg_gathering_check_instantiable,
      bool cfg_gathering_check_instance_callable,
      bool cfg_gathering_check_returning,
      bool remove_no_argument_constructors) override;
};
