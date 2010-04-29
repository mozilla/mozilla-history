/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

// This just verifies that the EM can actually startup and shutdown a few times
// without any errors

// We have to look up how many add-ons are present in case apps like SeaMonkey
// try to use this test with their built-in add-ons.
var gCount;

function run_test() {
  do_test_pending();
  createAppInfo("xpcshell@tests.mozilla.org", "XPCShell", "1", "1.9.2");

  var count = 0;
  startupManager(1);
  AddonManager.getAddonsByTypes(null, function(list) {
    gCount = list.length;

    run_test_1();
  });
}

function run_test_1() {
  restartManager(0);

  AddonManager.getAddonsByTypes(null, function(addons) {
    do_check_eq(gCount, addons.length);

    AddonManager.getAddonsWithPendingOperations(null, function(pendingAddons) {
      do_check_eq(0, pendingAddons.length);

      run_test_2();
    });
  });
}

function run_test_2() {
  shutdownManager();

  startupManager(0, false);

  AddonManager.getAddonsByTypes(null, function(addons) {
    do_check_eq(gCount, addons.length);

    run_test_3();
  });
}

function run_test_3() {
  restartManager(0);

  AddonManager.getAddonsByTypes(null, function(addons) {
    do_check_eq(gCount, addons.length);
    shutdownManager();
    do_test_finished();
  });
}
