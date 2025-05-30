/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <unordered_set>

#include "Debug.h"
#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Visitor.h"

#include "ApkResources.h"
#include "RedexMappedFile.h"
#include "RedexTest.h"
#include "ResourcesTestDefs.h"

namespace {

constexpr size_t NUM_GLOBAL_STRINGS = 6;
constexpr size_t NUM_KEY_STRINGS = 3;

class TypeVisitor : public arsc::ResourceTableVisitor {
 public:
  ~TypeVisitor() override {}

  bool visit_table(android::ResTable_header* table) override {
    ResourceTableVisitor::visit_table(table);
    m_package_count = dtohl(table->packageCount);
    return true;
  }

  bool visit_global_strings(android::ResStringPool_header* pool) override {
    ResourceTableVisitor::visit_global_strings(pool);
    m_global_strings_count = dtohl(pool->stringCount);
    m_style_count = dtohl(pool->styleCount);
    EXPECT_GT(dtohl(pool->stringsStart), 0) << "Should have style offset";
    return true;
  }

  bool visit_package(android::ResTable_package* package) override {
    ResourceTableVisitor::visit_package(package);
    m_package_id = dtohl(package->id);
    return true;
  }

  bool visit_type_strings(android::ResTable_package* package,
                          android::ResStringPool_header* pool) override {
    ResourceTableVisitor::visit_type_strings(package, pool);
    m_type_strings_count = dtohl(pool->stringCount);
    EXPECT_EQ(dtohl(pool->styleCount), 0)
        << "No styles expected in type strings";
    EXPECT_EQ(dtohl(pool->stylesStart), 0)
        << "No styles expected in type strings";
    return true;
  }

  bool visit_key_strings(android::ResTable_package* package,
                         android::ResStringPool_header* pool) override {
    ResourceTableVisitor::visit_type_strings(package, pool);
    m_key_strings_count = dtohl(pool->stringCount);
    EXPECT_EQ(dtohl(pool->styleCount), 0)
        << "No styles expected in key strings";
    EXPECT_EQ(dtohl(pool->stylesStart), 0)
        << "No styles expected in key strings";
    return true;
  }

  bool visit_type_spec(android::ResTable_package* package,
                       android::ResTable_typeSpec* type_spec) override {
    ResourceTableVisitor::visit_type_spec(package, type_spec);
    m_type_spec_entry_count = dtohl(type_spec->entryCount);
    EXPECT_EQ(type_spec->id, 1) << "table has 1 type in it, ID should be 1";
    return true;
  }

  bool visit_type(android::ResTable_package* package,
                  android::ResTable_typeSpec* type_spec,
                  android::ResTable_type* type) override {
    ResourceTableVisitor::visit_type(package, type_spec, type);
    m_type_entry_count = dtohl(type->entryCount);
    return true;
  }

  bool visit_entry(android::ResTable_package* /* unused */,
                   android::ResTable_typeSpec* /* unused */,
                   android::ResTable_type* /* unused */,
                   android::ResTable_entry* entry,
                   android::Res_value* value) override {
    m_entries++;
    EXPECT_LT(dtohl(entry->key.index), m_key_strings_count)
        << "Key index out of range";
    EXPECT_EQ(dtohs(value->size), 8);
    return true;
  }

  bool visit_map_entry(android::ResTable_package* /* unused */,
                       android::ResTable_typeSpec* /* unused */,
                       android::ResTable_type* /* unused */,
                       android::ResTable_map_entry* entry) override {
    m_map_entries++;
    EXPECT_LT(dtohl(entry->key.index), m_key_strings_count)
        << "Key index out of range";
    return true;
  }

  uint32_t m_package_count = 0;
  uint32_t m_global_strings_count = 0;
  uint32_t m_key_strings_count = 0;
  uint32_t m_type_strings_count = 0;
  uint32_t m_style_count = 0;
  uint32_t m_package_id = 0;
  uint32_t m_type_spec_entry_count = 0;
  uint32_t m_type_entry_count = 0;
  size_t m_entries = 0, m_map_entries = 0;
};

class StringTestVisitor : public arsc::StringPoolRefVisitor {
 public:
  ~StringTestVisitor() override {}

  bool visit_key_strings_ref(android::ResTable_package* /* unused */,
                             android::ResTable_type* /* unused */,
                             android::ResStringPool_ref* ref) override {
    m_key_strings_seen.emplace(dtohl(ref->index));
    return true;
  }

  bool visit_global_strings_ref(android::Res_value* value) override {
    m_global_strings_seen.emplace(dtohl(value->data));
    return true;
  }

  bool visit_global_strings_ref(android::ResStringPool_ref* ref) override {
    m_global_strings_seen.emplace(dtohl(ref->index));
    return true;
  }

  std::unordered_set<uint32_t> m_global_strings_seen;
  std::unordered_set<uint32_t> m_key_strings_seen;
};

class XmlStringCollector : public arsc::SimpleXmlParser {
 public:
  ~XmlStringCollector() override {}

  XmlStringCollector() {}

  std::string get_global_string(const android::ResStringPool_ref& ref) {
    auto idx = dtohl(ref.index);
    size_t len;
    auto& pool = global_strings();
    auto chars = pool.stringAt(idx, &len);
    always_assert_log(chars != nullptr, "invalid string ref %d", idx);
    android::String16 s16(chars, len);
    android::String8 s8(s16);
    return std::string(s8.string());
  }

  bool visit_string_ref(android::ResStringPool_ref* ref) override {
    auto ret = SimpleXmlParser::visit_string_ref(ref);
    auto idx = dtohl(ref->index);
    if (idx != 0xFFFFFFFF) {
      auto str = get_global_string(*ref);
      m_encountered_strings[str]++;
    }
    return ret;
  }

  std::unordered_map<std::string, size_t> m_encountered_strings;
};

class OverlayableIdsCollector : public arsc::ResourceTableVisitor {
 public:
  ~OverlayableIdsCollector() override {}

  OverlayableIdsCollector() {}

  bool visit_overlayable_id(
      android::ResTable_package* /* unused */,
      android::ResTable_overlayable_header* /* unused */,
      android::ResTable_overlayable_policy_header* /* unused */,
      uint32_t id) override {
    m_ids.emplace(id);
    return true;
  }

  std::unordered_set<uint32_t> m_ids;
};
} // namespace

TEST(Visitor, ParsePackageAndTypes) {
  auto f = RedexMappedFile::open(get_env("arsc_path"));
  TypeVisitor visitor;
  visitor.visit(const_cast<char*>(f.const_data()), f.size());
  EXPECT_EQ(visitor.m_package_count, 1) << "Should have only 1 package";
  EXPECT_EQ(visitor.m_global_strings_count, NUM_GLOBAL_STRINGS);
  EXPECT_EQ(visitor.m_key_strings_count, NUM_KEY_STRINGS);
  EXPECT_EQ(visitor.m_type_strings_count, 1);
  EXPECT_EQ(visitor.m_style_count, 2)
      << "Wrong style count in global pool header";
  EXPECT_EQ(visitor.m_package_id, 0x7f);
  EXPECT_EQ(visitor.m_type_spec_entry_count, 3);
  EXPECT_EQ(visitor.m_type_entry_count, 3);
  EXPECT_EQ(visitor.m_entries, 3);
  EXPECT_EQ(visitor.m_map_entries, 0);
}

TEST(Visitor, VisitAllStrings) {
  auto f = RedexMappedFile::open(get_env("arsc_path"));
  StringTestVisitor visitor;
  visitor.visit(const_cast<char*>(f.const_data()), f.size());
  EXPECT_EQ(visitor.m_global_strings_seen.size(), NUM_GLOBAL_STRINGS)
      << "Not all global strings visited!";
  for (size_t i = 0; i < NUM_GLOBAL_STRINGS; i++) {
    EXPECT_EQ(visitor.m_global_strings_seen.count(i), 1)
        << "Did not visit global string index " << i;
  }
  EXPECT_EQ(visitor.m_key_strings_seen.size(), NUM_KEY_STRINGS)
      << "Not all key strings visited!";
  for (size_t i = 0; i < NUM_KEY_STRINGS; i++) {
    EXPECT_EQ(visitor.m_key_strings_seen.count(i), 1)
        << "Did not visit key string index " << i;
  }
}

TEST(Visitor, VisitXmlStrings) {
  auto f = RedexMappedFile::open(get_env("xml_path"));
  XmlStringCollector collector;
  collector.visit(const_cast<char*>(f.const_data()), f.size());
  EXPECT_EQ(collector.m_encountered_strings.size(), 8);
  // Twice for the start/end node.
  EXPECT_EQ(collector.m_encountered_strings.at("Button"), 2);
  EXPECT_EQ(collector.m_encountered_strings.at("background"), 1);
  EXPECT_EQ(collector.m_encountered_strings.at("padding"), 1);
  EXPECT_EQ(collector.m_encountered_strings.at("layout_width"), 1);
  EXPECT_EQ(collector.m_encountered_strings.at("layout_height"), 1);
  EXPECT_EQ(collector.m_encountered_strings.at("text"), 1);
  // Twice for start/end namespace.
  EXPECT_EQ(collector.m_encountered_strings.at("android"), 2);
  // Twice for the start/end namespace, plus 5 for each attribute in the
  // namespace (note that attribute string ref points to the uri not the name).
  EXPECT_EQ(collector.m_encountered_strings.at(
                "http://schemas.android.com/apk/res/android"),
            7);
}

TEST(Visitor, VisitOverlayableIds) {
  auto arsc_path = get_env("test_res_path");
  ResourcesArscFile res_table(arsc_path);
  OverlayableIdsCollector collector;
  auto f = RedexMappedFile::open(arsc_path);
  collector.visit(const_cast<char*>(f.const_data()), f.size());
  EXPECT_EQ(collector.m_ids.size(),
            sample_app::EXPECTED_OVERLAYABLE_RESOURCES.size());
  for (auto& name : sample_app::EXPECTED_OVERLAYABLE_RESOURCES) {
    auto id = res_table.name_to_ids.at(name).at(0);
    EXPECT_TRUE(collector.m_ids.count(id) > 0)
        << "Did not find 0x" << std::hex << id;
  }
}
