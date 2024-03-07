/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Model.h"
#include "Pass.h"

namespace class_merging {

class AnonymousClassMergingPass : public Pass {
 public:
  AnonymousClassMergingPass() : Pass("AnonymousClassMergingPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
    };
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  ModelSpec m_merging_spec;
  size_t m_global_min_count;
  size_t m_min_count;
  size_t m_max_count;
};

} // namespace class_merging
