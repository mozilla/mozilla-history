SimpleTest.waitForExplicitFinish();

var gPopup = null;

var gExpectedCookies = 0;
var gExpectedLoads = 0;
var gLoads = 0;

function setupTest(uri, cookies, loads) {
  netscape.security.PrivilegeManager.enablePrivilege("UniversalXPConnect");

  Components.classes["@mozilla.org/preferences-service;1"]
            .getService(Components.interfaces.nsIPrefBranch)
            .setIntPref("network.cookie.cookieBehavior", 1);

  var cs = Components.classes["@mozilla.org/cookiemanager;1"]
                     .getService(Components.interfaces.nsICookieManager2);
  cs.removeAll();

  gExpectedCookies = cookies;
  gExpectedLoads = loads;

  // load a window which contains an iframe; each will attempt to set
  // cookies from their respective domains.
  gPopup = window.open(uri, 'hai', 'width=100,height=100');
}

window.addEventListener("message", messageReceiver, false);

/** Receives MessageEvents to this window. */
function messageReceiver(evt)
{
  ok(evt instanceof MessageEvent, "event type", evt);

  if (evt.data != "message") {
    window.removeEventListener("message", messageReceiver, false);

    ok(false, "message", evt.data);

    gPopup.close();
    SimpleTest.finish();
    return;
  }

  // only run the test when all our children are done loading & setting cookies
  if (++gLoads == gExpectedLoads) {
    window.removeEventListener("message", messageReceiver, false);

    runTest();
  }
}

function runTest() {
  // set a cookie from a domain of "localhost"
  document.cookie = "oh=hai";

  netscape.security.PrivilegeManager.enablePrivilege("UniversalXPConnect");

  var cs = Components.classes["@mozilla.org/cookiemanager;1"]
                     .getService(Components.interfaces.nsICookieManager);
  var list = cs.enumerator;
  var count = 0;
  while (list.hasMoreElements()) {
    count++;
    list.getNext();
  }
  is(count, gExpectedCookies, "number of cookies");
  cs.removeAll();

  gPopup.close();
  SimpleTest.finish();
}
