#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "../src/recover_flash.h"

struct TestPage {
  bool readOk = true;
  bool erased = true;
  bool valid = false;
  uint64_t unixOffset = 0;
};

struct TestContext {
  std::vector<TestPage> pages;
  TestPage current;
};

static bool testReadPage(void *ctx, uint16_t const flashPageID) {
  TestContext *const test = static_cast<TestContext *>(ctx);

  if (flashPageID >= test->pages.size()) {
    return false;
  }
  test->current = test->pages[flashPageID];
  return test->current.readOk;
}

static bool testIsErased(void *ctx) {
  return static_cast<TestContext *>(ctx)->current.erased;
}

static bool testIsValid(void *ctx) {
  return static_cast<TestContext *>(ctx)->current.valid;
}

static uint64_t testUnixOffset(void *ctx) {
  return static_cast<TestContext *>(ctx)->current.unixOffset;
}

static void quietLog(char const *const) {
}

static void require(bool const condition, char const *const message) {
  if (!condition) {
    fprintf(stderr, "TEST FAILURE: %s\n", message);
    exit(1);
  }
}

static RecoverFlashConfig makeConfig() {
  return {
    .flashSectStart = 0,
    .flashSectTotal = 4,
    .flashSectPageFactor = 2,
    .flashSectPageCount = 4,
    .flashPageExp = 8,
    .firstPageCountEmpty = 0,
  };
}

static void fillPages(TestContext &ctx, uint16_t const count, uint64_t const firstUnix, uint64_t const step) {
  uint16_t i;

  for (i = 0; i < count; ++i) {
    ctx.pages[i].erased = false;
    ctx.pages[i].valid = true;
    ctx.pages[i].unixOffset = firstUnix + uint64_t(i) * step;
  }
}

static void fillPagesAt(TestContext &ctx, uint16_t const start, uint16_t const count, uint64_t const firstUnix, uint64_t const step) {
  uint16_t i;

  for (i = 0; i < count; ++i) {
    ctx.pages[start + i].erased = false;
    ctx.pages[start + i].valid = true;
    ctx.pages[start + i].unixOffset = firstUnix + uint64_t(i) * step;
  }
}

static void test_empty_flash() {
  TestContext ctx = {.pages = std::vector<TestPage>(16)};
  RecoverFlashResult const result = recoverFlashLayout(makeConfig(), &ctx, testReadPage, testIsErased, testIsValid, testUnixOffset, quietLog);

  require(result.headL2 == 0, "empty flash head");
  require(result.countL2 == 0, "empty flash count");
}

static void test_prefix_only() {
  TestContext ctx = {.pages = std::vector<TestPage>(16)};
  RecoverFlashResult result;

  fillPages(ctx, 6, 1000, 100);
  result = recoverFlashLayout(makeConfig(), &ctx, testReadPage, testIsErased, testIsValid, testUnixOffset, quietLog);

  require(result.headL2 == 0, "prefix flash head");
  require(result.countL2 == 6, "prefix flash count");
}

static void test_full_wrap() {
  TestContext ctx = {.pages = std::vector<TestPage>(16)};
  RecoverFlashResult result;

  fillPagesAt(ctx, 0, 8, 900, 100);
  fillPagesAt(ctx, 8, 8, 100, 100);
  result = recoverFlashLayout(makeConfig(), &ctx, testReadPage, testIsErased, testIsValid, testUnixOffset, quietLog);

  require(result.headL2 == 2, "wrapped flash head");
  require(result.countL2 == 16, "wrapped flash count");
}

static void test_bad_page_falls_back_to_prefix() {
  TestContext ctx = {.pages = std::vector<TestPage>(16)};
  RecoverFlashResult result;

  fillPages(ctx, 6, 1000, 100);
  ctx.pages[6].erased = false;
  ctx.pages[6].valid = false;
  result = recoverFlashLayout(makeConfig(), &ctx, testReadPage, testIsErased, testIsValid, testUnixOffset, quietLog);

  require(result.headL2 == 0, "bad page prefix head");
  require(result.countL2 == 4, "bad page prefix count");
}

int main() {
  test_empty_flash();
  test_prefix_only();
  test_full_wrap();
  test_bad_page_falls_back_to_prefix();
  return 0;
}
