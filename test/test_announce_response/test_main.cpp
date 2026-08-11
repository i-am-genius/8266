#include <cstring>

#include <unity.h>

#include "network/announce_response.h"

void setUp() {}
void tearDown() {}

void test_reads_added_true_from_common_result_data() {
  const char* payload = R"({"code":200,"msg":"ok","data":{"added":true}})";
  bool added = false;

  AnnounceResponseStatus status = parseAnnounceAdded(payload, strlen(payload), added);

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(AnnounceResponseStatus::Parsed),
    static_cast<int>(status)
  );
  TEST_ASSERT_TRUE(added);
}

void test_reads_added_false_from_common_result_data() {
  const char* payload = R"({"code":200,"msg":"ok","data":{"added":false}})";
  bool added = true;

  AnnounceResponseStatus status = parseAnnounceAdded(payload, strlen(payload), added);

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(AnnounceResponseStatus::Parsed),
    static_cast<int>(status)
  );
  TEST_ASSERT_FALSE(added);
}

void test_rejects_text_that_only_contains_added_substring() {
  const char* payload = R"({"message":"cached text: \"added\":true"})";
  bool added = true;

  AnnounceResponseStatus status = parseAnnounceAdded(payload, strlen(payload), added);

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(AnnounceResponseStatus::MissingAdded),
    static_cast<int>(status)
  );
  TEST_ASSERT_FALSE(added);
}

void test_rejects_invalid_json() {
  const char* payload = "not-json";
  bool added = true;

  AnnounceResponseStatus status = parseAnnounceAdded(payload, strlen(payload), added);

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(AnnounceResponseStatus::InvalidJson),
    static_cast<int>(status)
  );
  TEST_ASSERT_FALSE(added);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reads_added_true_from_common_result_data);
  RUN_TEST(test_reads_added_false_from_common_result_data);
  RUN_TEST(test_rejects_text_that_only_contains_added_substring);
  RUN_TEST(test_rejects_invalid_json);
  return UNITY_END();
}
