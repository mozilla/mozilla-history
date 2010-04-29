/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

// This verifies startup detection of added/removed/changed items and install
// location priorities

// We try to count the non-test extensions the add-on manager detects like the
// built-in extensions in seamonkey.
var additionalAddons, additionalExtensions;

var addon1 = {
  id: "addon1@tests.mozilla.org",
  version: "1.0",
  name: "Test 1",
  targetApplications: [{
    id: "xpcshell@tests.mozilla.org",
    minVersion: "1",
    maxVersion: "1"
  }]
};

var addon2 = {
  id: "addon2@tests.mozilla.org",
  version: "2.0",
  name: "Test 2",
  targetApplications: [{
    id: "xpcshell@tests.mozilla.org",
    minVersion: "1",
    maxVersion: "2"
  }]
};

var addon3 = {
  id: "addon3@tests.mozilla.org",
  version: "3.0",
  name: "Test 3",
  targetApplications: [{
    id: "toolkit@mozilla.org",
    minVersion: "1.9.2",
    maxVersion: "1.9.2.*"
  }]
};

// Should be ignored because it has no ID
var addon4 = {
  version: "4.0",
  name: "Test 4",
  targetApplications: [{
    id: "xpcshell@tests.mozilla.org",
    minVersion: "1",
    maxVersion: "1"
  }]
};

// Should be ignored because it has no version
var addon5 = {
  id: "addon5@tests.mozilla.org",
  name: "Test 5",
  targetApplications: [{
    id: "xpcshell@tests.mozilla.org",
    minVersion: "1",
    maxVersion: "1"
  }]
};

createAppInfo("xpcshell@tests.mozilla.org", "XPCShell", "1", "1.9.2");

const globalDir = gProfD.clone();
globalDir.append("extensions2");
globalDir.append(gAppInfo.ID);
registerDirectory("XRESysSExtPD", globalDir.parent);
const userDir = gProfD.clone();
userDir.append("extensions3");
userDir.append(gAppInfo.ID);
registerDirectory("XREUSysExt", userDir.parent);
const profileDir = gProfD.clone();
profileDir.append("extensions");

// Set up the profile
function run_test() {
  do_test_pending();
  startupManager(1);

  AddonManager.getAddonsByTypes(null, function(allAddons) {
    additionalAddons = allAddons.length;
    AddonManager.getAddonsByTypes(["extension"], function(allExtensions) {
      additionalExtensions = allExtensions.length;
      AddonManager.getAddons(["addon1@tests.mozilla.org",
                              "addon2@tests.mozilla.org",
                              "addon3@tests.mozilla.org",
                              "addon4@tests.mozilla.org",
                              "addon5@tests.mozilla.org"],
                              function([a1, a2, a3, a4, a5]) {

        do_check_eq(a1, null);
        do_check_eq(a2, null);
        do_check_eq(a3, null);
        do_check_eq(a4, null);
        do_check_eq(a5, null);

        run_test_1();
      });
    });
  });
}

function end_test() {
  do_test_finished();
}

// Try to install all the items into the profile
function run_test_1() {
  var dest = profileDir.clone();
  dest.append("addon1@tests.mozilla.org");
  writeInstallRDFToDir(addon1, dest);
  dest = profileDir.clone();
  dest.append("addon2@tests.mozilla.org");
  writeInstallRDFToDir(addon2, dest);
  dest = profileDir.clone();
  dest.append("addon3@tests.mozilla.org");
  writeInstallRDFToDir(addon3, dest);
  dest = profileDir.clone();
  dest.append("addon4@tests.mozilla.org");
  writeInstallRDFToDir(addon4, dest);
  dest = profileDir.clone();
  dest.append("addon5@tests.mozilla.org");
  writeInstallRDFToDir(addon5, dest);

  restartManager(1);
  AddonManager.getAddons(["addon1@tests.mozilla.org",
                          "addon2@tests.mozilla.org",
                          "addon3@tests.mozilla.org",
                          "addon4@tests.mozilla.org",
                          "addon5@tests.mozilla.org"],
                          function([a1, a2, a3, a4, a5]) {

    do_check_neq(a1, null);
    do_check_eq(a1.id, "addon1@tests.mozilla.org");
    do_check_eq(a1.version, "1.0");
    do_check_eq(a1.name, "Test 1");
    do_check_true(isExtensionInAddonsList(profileDir, a1.id));
    do_check_true(hasFlag(a1.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a1.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_neq(a2, null);
    do_check_eq(a2.id, "addon2@tests.mozilla.org");
    do_check_eq(a2.version, "2.0");
    do_check_eq(a2.name, "Test 2");
    do_check_true(isExtensionInAddonsList(profileDir, a2.id));
    do_check_true(hasFlag(a2.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a2.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_neq(a3, null);
    do_check_eq(a3.id, "addon3@tests.mozilla.org");
    do_check_eq(a3.version, "3.0");
    do_check_eq(a3.name, "Test 3");
    do_check_true(isExtensionInAddonsList(profileDir, a3.id));
    do_check_true(hasFlag(a3.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a3.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_eq(a4, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon4@tests.mozilla.org"));
    dest = profileDir.clone();
    dest.append("addon4@tests.mozilla.org");
    do_check_false(dest.exists());

    do_check_eq(a5, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon5@tests.mozilla.org"));
    dest = profileDir.clone();
    dest.append("addon5@tests.mozilla.org");
    do_check_false(dest.exists());

    AddonManager.getAddonsByTypes(null, function(addons) {
      do_check_eq(addons.length, 3 + additionalAddons);
      AddonManager.getAddonsByTypes(["extension"], function(extensionAddons) {
        do_check_eq(extensionAddons.length, 3 + additionalExtensions);
        run_test_2();
      });
    });
  });
}

// Test that modified items are detected and items in other install locations
// are ignored
function run_test_2() {
  var dest = userDir.clone();
  dest.append("addon1@tests.mozilla.org");
  addon1.version = "1.1";
  writeInstallRDFToDir(addon1, dest);
  dest = profileDir.clone();
  dest.append("addon2@tests.mozilla.org");
  addon2.version="2.1";
  writeInstallRDFToDir(addon2, dest);
  dest = globalDir.clone();
  dest.append("addon2@tests.mozilla.org");
  addon2.version="2.2";
  writeInstallRDFToDir(addon2, dest);
  dest = userDir.clone();
  dest.append("addon2@tests.mozilla.org");
  addon2.version="2.3";
  writeInstallRDFToDir(addon2, dest);
  dest = profileDir.clone();
  dest.append("addon3@tests.mozilla.org");
  dest.remove(true);

  restartManager(1);

  AddonManager.getAddons(["addon1@tests.mozilla.org",
                          "addon2@tests.mozilla.org",
                          "addon3@tests.mozilla.org",
                          "addon4@tests.mozilla.org",
                          "addon5@tests.mozilla.org"],
                          function([a1, a2, a3, a4, a5]) {

    do_check_neq(a1, null);
    do_check_eq(a1.id, "addon1@tests.mozilla.org");
    do_check_eq(a1.version, "1.0");
    do_check_true(isExtensionInAddonsList(profileDir, a1.id));
    do_check_false(isExtensionInAddonsList(userDir, a1.id));
    do_check_true(hasFlag(a1.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a1.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_neq(a2, null);
    do_check_eq(a2.id, "addon2@tests.mozilla.org");
    do_check_eq(a2.version, "2.1");
    do_check_true(isExtensionInAddonsList(profileDir, a2.id));
    do_check_false(isExtensionInAddonsList(userDir, a2.id));
    do_check_false(isExtensionInAddonsList(globalDir, a2.id));
    do_check_true(hasFlag(a2.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a2.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_eq(a3, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon3@tests.mozilla.org"));

    do_check_eq(a4, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon4@tests.mozilla.org"));

    do_check_eq(a5, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon5@tests.mozilla.org"));

    run_test_3();
  });
}

// Check that removing items from the profile reveals their hidden versions.
function run_test_3() {
  var dest = profileDir.clone();
  dest.append("addon1@tests.mozilla.org");
  dest.remove(true);
  dest = profileDir.clone();
  dest.append("addon2@tests.mozilla.org");
  dest.remove(true);
  dest = profileDir.clone();
  dest.append("addon4@tests.mozilla.org");
  writeInstallRDFToDir(addon3, dest);

  restartManager(1);

  AddonManager.getAddons(["addon1@tests.mozilla.org",
                          "addon2@tests.mozilla.org",
                          "addon3@tests.mozilla.org",
                          "addon4@tests.mozilla.org",
                          "addon5@tests.mozilla.org"],
                          function([a1, a2, a3, a4, a5]) {

    do_check_neq(a1, null);
    do_check_eq(a1.id, "addon1@tests.mozilla.org");
    do_check_eq(a1.version, "1.1");
    do_check_false(isExtensionInAddonsList(profileDir, a1.id));
    do_check_true(isExtensionInAddonsList(userDir, a1.id));
    do_check_false(hasFlag(a1.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_false(hasFlag(a1.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_neq(a2, null);
    do_check_eq(a2.id, "addon2@tests.mozilla.org");
    do_check_eq(a2.version, "2.3");
    do_check_false(isExtensionInAddonsList(profileDir, a2.id));
    do_check_true(isExtensionInAddonsList(userDir, a2.id));
    do_check_false(isExtensionInAddonsList(globalDir, a2.id));
    do_check_false(hasFlag(a2.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_false(hasFlag(a2.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_eq(a3, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon3@tests.mozilla.org"));

    do_check_eq(a4, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon4@tests.mozilla.org"));

    do_check_eq(a5, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon5@tests.mozilla.org"));

    dest = profileDir.clone();
    dest.append("addon4@tests.mozilla.org");
    do_check_false(dest.exists());

    run_test_4();
  });
}

// Check that items in the profile hide the others again.
function run_test_4() {
  var dest = profileDir.clone();
  dest.append("addon1@tests.mozilla.org");
  addon1.version = "1.2";
  writeInstallRDFToDir(addon1, dest);
  dest = userDir.clone();
  dest.append("addon2@tests.mozilla.org");
  dest.remove(true);

  restartManager(1);

  AddonManager.getAddons(["addon1@tests.mozilla.org",
                          "addon2@tests.mozilla.org",
                          "addon3@tests.mozilla.org",
                          "addon4@tests.mozilla.org",
                          "addon5@tests.mozilla.org"],
                          function([a1, a2, a3, a4, a5]) {

    do_check_neq(a1, null);
    do_check_eq(a1.id, "addon1@tests.mozilla.org");
    do_check_eq(a1.version, "1.2");
    do_check_true(isExtensionInAddonsList(profileDir, a1.id));
    do_check_false(isExtensionInAddonsList(userDir, a1.id));
    do_check_true(hasFlag(a1.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a1.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_neq(a2, null);
    do_check_eq(a2.id, "addon2@tests.mozilla.org");
    do_check_eq(a2.version, "2.2");
    do_check_false(isExtensionInAddonsList(profileDir, a2.id));
    do_check_false(isExtensionInAddonsList(userDir, a2.id));
    do_check_true(isExtensionInAddonsList(globalDir, a2.id));
    do_check_false(hasFlag(a2.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_false(hasFlag(a2.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_eq(a3, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon3@tests.mozilla.org"));

    do_check_eq(a4, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon4@tests.mozilla.org"));

    do_check_eq(a5, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon5@tests.mozilla.org"));

    run_test_5();
  });
}

// More hiding and revealing
function run_test_5() {
  var dest = userDir.clone();
  dest.append("addon1@tests.mozilla.org");
  dest.remove(true);
  dest = globalDir.clone();
  dest.append("addon2@tests.mozilla.org");
  dest.remove(true);
  dest = profileDir.clone();
  dest.append("addon2@tests.mozilla.org");
  addon2.version = "2.4";
  writeInstallRDFToDir(addon2, dest);

  restartManager(1);

  AddonManager.getAddons(["addon1@tests.mozilla.org",
                          "addon2@tests.mozilla.org",
                          "addon3@tests.mozilla.org",
                          "addon4@tests.mozilla.org",
                          "addon5@tests.mozilla.org"],
                          function([a1, a2, a3, a4, a5]) {

    do_check_neq(a1, null);
    do_check_eq(a1.id, "addon1@tests.mozilla.org");
    do_check_eq(a1.version, "1.2");
    do_check_true(isExtensionInAddonsList(profileDir, a1.id));
    do_check_false(isExtensionInAddonsList(userDir, a1.id));
    do_check_true(hasFlag(a1.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a1.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_neq(a2, null);
    do_check_eq(a2.id, "addon2@tests.mozilla.org");
    do_check_eq(a2.version, "2.4");
    do_check_true(isExtensionInAddonsList(profileDir, a2.id));
    do_check_false(isExtensionInAddonsList(userDir, a2.id));
    do_check_false(isExtensionInAddonsList(globalDir, a2.id));
    do_check_true(hasFlag(a2.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a2.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_eq(a3, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon3@tests.mozilla.org"));

    do_check_eq(a4, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon4@tests.mozilla.org"));

    do_check_eq(a5, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon5@tests.mozilla.org"));

    run_test_6();
  });
}

// Checks that a removal from one location and an addition in another location
// for the same item is handled
function run_test_6() {
  var dest = profileDir.clone();
  dest.append("addon1@tests.mozilla.org");
  dest.remove(true);
  dest = userDir.clone();
  dest.append("addon1@tests.mozilla.org");
  addon1.version = "1.3";
  writeInstallRDFToDir(addon1, dest);

  restartManager(1);

  AddonManager.getAddons(["addon1@tests.mozilla.org",
                          "addon2@tests.mozilla.org",
                          "addon3@tests.mozilla.org",
                          "addon4@tests.mozilla.org",
                          "addon5@tests.mozilla.org"],
                          function([a1, a2, a3, a4, a5]) {

    do_check_neq(a1, null);
    do_check_eq(a1.id, "addon1@tests.mozilla.org");
    do_check_eq(a1.version, "1.3");
    do_check_false(isExtensionInAddonsList(profileDir, a1.id));
    do_check_true(isExtensionInAddonsList(userDir, a1.id));
    do_check_false(hasFlag(a1.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_false(hasFlag(a1.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_neq(a2, null);
    do_check_eq(a2.id, "addon2@tests.mozilla.org");
    do_check_eq(a2.version, "2.4");
    do_check_true(isExtensionInAddonsList(profileDir, a2.id));
    do_check_false(isExtensionInAddonsList(userDir, a2.id));
    do_check_false(isExtensionInAddonsList(globalDir, a2.id));
    do_check_true(hasFlag(a2.permissions, AddonManager.PERM_CAN_UNINSTALL));
    do_check_true(hasFlag(a2.permissions, AddonManager.PERM_CAN_UPGRADE));

    do_check_eq(a3, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon3@tests.mozilla.org"));

    do_check_eq(a4, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon4@tests.mozilla.org"));

    do_check_eq(a5, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon5@tests.mozilla.org"));

    run_test_7();
  });
}

// This should remove any remaining items
function run_test_7() {
  var dest = userDir.clone();
  dest.append("addon1@tests.mozilla.org");
  dest.remove(true);
  dest = profileDir.clone();
  dest.append("addon2@tests.mozilla.org");
  dest.remove(true);

  restartManager(1);

  AddonManager.getAddons(["addon1@tests.mozilla.org",
                          "addon2@tests.mozilla.org",
                          "addon3@tests.mozilla.org",
                          "addon4@tests.mozilla.org",
                          "addon5@tests.mozilla.org"],
                          function([a1, a2, a3, a4, a5]) {

    do_check_eq(a1, null);
    do_check_eq(a2, null);
    do_check_eq(a3, null);
    do_check_eq(a4, null);
    do_check_eq(a5, null);
    do_check_false(isExtensionInAddonsList(profileDir, "addon1@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(profileDir, "addon2@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(profileDir, "addon3@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(profileDir, "addon4@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(profileDir, "addon5@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(userDir, "addon1@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(userDir, "addon2@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(userDir, "addon3@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(userDir, "addon4@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(userDir, "addon5@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(globalDir, "addon1@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(globalDir, "addon2@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(globalDir, "addon3@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(globalDir, "addon4@tests.mozilla.org"));
    do_check_false(isExtensionInAddonsList(globalDir, "addon5@tests.mozilla.org"));

    end_test();
  });
}
