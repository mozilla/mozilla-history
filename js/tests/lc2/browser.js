/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
var completed = false;
var testcases;

var TT = "<tt>";
var TT_ = "<tt>";
var BR = "<br>";
var NBSP = "&nbsp;";
var CR = "\n";
var FONT = "";
var FONT_ = "</font>";
var FONT_RED = "<font color=\"red\">";
var FONT_GREEN = "<font color=\"green\">";
var B = "<b>";
var B_ = "</b>"
var H2 = "<h2>";
var H2_ = "</h2>";
var HR = "<hr>";

function htmlesc(str) { 
  if (str == '<') 
    return '&lt;'; 
  if (str == '>') 
    return '&gt;'; 
  if (str == '&') 
    return '&amp;'; 
  return str; 
}

function DocumentWrite(s)
{
  try
  {
    var msgDiv = document.createElement('div');
    msgDiv.innerHTML = s;
    document.body.appendChild(msgDiv);
    msgDiv = null;
  }
  catch(excp)
  {
    document.write(s + '<br>\n');
  }
}

function print() { 
  var s = ''; 
  var a;
  for (var i = 0; i < arguments.length; i++) 
  { 
    a = arguments[i]; 
    s += String(a) + ' '; 
  } 

  if (typeof dump == 'function')
  {
    dump( s + '\n');
  }

  s = s.replace(/[<>&]/g, htmlesc);

  DocumentWrite(s);
}

function writeHeaderToLog( string ) {
  string = String(string);

  if (typeof dump == 'function')
  {
    dump( string + '\n');
  }

  string = string.replace(/[<>&]/g, htmlesc);

  DocumentWrite( "<h2>" + string + "</h2>" );
}

function writeFormattedResult( expect, actual, string, passed ) {
  string = String(string);

  if (typeof dump == 'function')
  {
    dump( string + '\n');
  }

  string = string.replace(/[<>&]/g, htmlesc);

  var s = "<tt>"+ string ;
  s += "<b>" ;
  s += ( passed ) ? "<font color=#009900> &nbsp;" + PASSED
    : "<font color=#aa0000>&nbsp;" +  FAILED + expect + "</tt>";

  DocumentWrite( s + "</font></b></tt><br>" );
  return passed;
}

function ToInteger( t ) {
  t = Number( t );

  if ( isNaN( t ) ){
    return ( Number.NaN );
  }
  if ( t == 0 || t == -0 ||
       t == Number.POSITIVE_INFINITY || t == Number.NEGATIVE_INFINITY ) {
    return 0;
  }

  var sign = ( t < 0 ) ? -1 : 1;

  return ( sign * Math.floor( Math.abs( t ) ) );
}
function Enumerate ( o ) {
  var p;
  for ( p in o ) {
    print( p +": " + o[p] );
  }
}

/*
 * The earlier versions of the test code used exceptions
 * to terminate the test script in "negative" test cases
 * before the failure reporting code could run. In order
 * to be able to capture errors for the "negative" case 
 * where the exception is a sign the test actually passed,
 * the err online handler will assume that any error is a 
 * failure unless gExceptionExpected is true.
 */
window.onerror = err;
var gExceptionExpected = false;

function err( msg, page, line ) {
  var testcase;

  optionsPush();

  if (typeof(EXPECTED) == "undefined" || EXPECTED != "error") {
/*
 * an unexpected exception occured
 */
    print( "Test failed with the message: " + msg );
    testcase = new TestCase(SECTION, "unknown", "unknown", "unknown");
    testcase.passed = false;
    testcase.reason = "Error: " + msg + 
	    " Source File: " + page + " Line: " + line + ".";
    if (document.location.href.indexOf('-n.js') != -1)
    {
      // negative test
      testcase.passed = true;
    }
  }
  else
  {
    if (typeof SECTION == 'undefined')
    {
      SECTION = 'Unknown';
    }
    if (typeof DESCRIPTION == 'undefined')
    {
      DESCRIPTION = 'Unknown';
    }
    if (typeof EXPECTED == 'undefined')
    {
      EXPECTED = 'Unknown';
    }

    testcase = new TestCase(SECTION, DESCRIPTION, EXPECTED, "error");
    testcase.reason += "Error: " + msg + 
      " Source File: " + page + " Line: " + line + ".";
    stopTest();

    gDelayTestDriverEnd = false;
    jsTestDriverEnd();
  }

  optionsReset();
}

var gVersion = 0;

function version(v)
{
  if (v) { 
    gVersion = v; 
  } 
  return gVersion; 
}

function gc()
{
  // Thanks to igor.bukanov@gmail.com
  for (var i = 0; i != 100000; ++i)
  {
    var tmp = new Object();
  }
}

function jsdgc()
{
  try
  {
    // Thanks to dveditz
    netscape.security.PrivilegeManager.enablePrivilege('UniversalXPConnect');
    var jsdIDebuggerService = Components.interfaces.jsdIDebuggerService;
    var service = Components.classes['@mozilla.org/js/jsd/debugger-service;1'].
      getService(jsdIDebuggerService);
    service.GC();
  }
  catch(ex)
  {
    print('gc: ' + ex);
  }
}

function Preferences(aPrefRoot)
{
  try
  {
    this.orig = {};
    this.privs = 'UniversalXPConnect UniversalPreferencesRead ' + 
      'UniversalPreferencesWrite';

    if (typeof netscape != 'undefined' &&
        'security' in netscape &&
        'PrivilegeManager' in netscape.security &&
        'enablePrivilege' in netscape.security.PrivilegeManager)
    {
      netscape.security.PrivilegeManager.enablePrivilege(this.privs);
    }

    const nsIPrefService = Components.interfaces.nsIPrefService;
    const nsIPrefBranch = Components.interfaces.nsIPrefBranch;
    const nsPrefService_CONTRACTID = "@mozilla.org/preferences-service;1";

    this.prefRoot    = aPrefRoot;
    this.prefService = Components.classes[nsPrefService_CONTRACTID].
      getService(nsIPrefService);
    this.prefBranch = this.prefService.getBranch(aPrefRoot).
      QueryInterface(Components.interfaces.nsIPrefBranch2);
  }
  catch(ex)
  {
  }

}

function Preferences_getPrefRoot() 
{
  try
  {
    if (typeof netscape != 'undefined' &&
        'security' in netscape &&
        'PrivilegeManager' in netscape.security &&
        'enablePrivilege' in netscape.security.PrivilegeManager)
    {
      netscape.security.PrivilegeManager.enablePrivilege(this.privs);
    }

    return this.prefBranch.root; 
  }
  catch(ex)
  {
    return;
  }
}

function Preferences_getPref(aPrefName) 
{
  var value;
  try
  {
    if (typeof netscape != 'undefined' &&
        'security' in netscape &&
        'PrivilegeManager' in netscape.security &&
        'enablePrivilege' in netscape.security.PrivilegeManager)
    {
      netscape.security.PrivilegeManager.enablePrivilege(this.privs);
    }

    try
    {    
      value = this.prefBranch.getBoolPref(aPrefName);
    }
    catch(ex)
    {
      //print('Ignoring ' + ex);
    }
  }
  catch(ex)
  {
  }
  return value;
}

function Preferences_setPref(aPrefName, aPrefValue) 
{
  try
  {
    if (typeof netscape != 'undefined' &&
        'security' in netscape &&
        'PrivilegeManager' in netscape.security &&
        'enablePrivilege' in netscape.security.PrivilegeManager)
    {
      netscape.security.PrivilegeManager.enablePrivilege(this.privs);
    }

    if (typeof this.orig[aPrefName] == 'undefined')
    {
      this.orig[aPrefName] = this.getPref(aPrefName);
    }

    try
    {
      value = this.prefBranch.setBoolPref(aPrefName, aPrefValue);
    }
    catch(ex)
    {
      //print('Ignoring ' + ex);
    }
  }
  catch(ex)
  {
  }
}

function Preferences_resetPref(aPrefName) 
{
  try
  {
    if (typeof netscape != 'undefined' &&
        'security' in netscape &&
        'PrivilegeManager' in netscape.security &&
        'enablePrivilege' in netscape.security.PrivilegeManager)
    {
      netscape.security.PrivilegeManager.enablePrivilege(this.privs);
    }

    if (aPrefName in this.orig)
    {
      this.setPref(aPrefName, this.orig[aPrefName]);
    }
  }
  catch(ex)
  {
  }
}

function Preferences_resetAllPrefs() 
{
  try
  {
    var prefName;
    var prefValue;

    if (typeof netscape != 'undefined' &&
        'security' in netscape &&
        'PrivilegeManager' in netscape.security &&
        'enablePrivilege' in netscape.security.PrivilegeManager)
    {
      netscape.security.PrivilegeManager.enablePrivilege(this.privs);
    }

    for (prefName in this.orig)
    {
      this.setPref(prefName, this.orig[prefName]);
    }
  }
  catch(ex)
  {
  }
}

function Preferences_clearPref(aPrefName) 
{
  try
  {
    if (typeof netscape != 'undefined' &&
        'security' in netscape &&
        'PrivilegeManager' in netscape.security &&
        'enablePrivilege' in netscape.security.PrivilegeManager)
    {
      netscape.security.PrivilegeManager.enablePrivilege(this.privs);
    }

    this.prefBranch.clearUserPref(aPrefName);
  }
  catch(ex)
  {
  }
}

Preferences.prototype.getPrefRoot    = Preferences_getPrefRoot;
Preferences.prototype.getPref        = Preferences_getPref;
Preferences.prototype.setPref        = Preferences_setPref;
Preferences.prototype.resetAllPrefs  = Preferences_resetAllPrefs;
Preferences.prototype.resetPref      = Preferences_resetPref;
Preferences.prototype.clearPref      = Preferences_clearPref;

function options(aOptionName) 
{
  // return value of options() is a comma delimited list
  // of the previously set values

  var value = '';
  for (var optionName in options.currvalues)
  {
    value += optionName + ',';
  }
  if (value)
  {
    value = value.substring(0, value.length-1);
  }

  if (aOptionName)
  {
    if (options.currvalues[aOptionName])
    {
      // option is set, toggle it to unset
      delete options.currvalues[aOptionName];
      options.preferences.setPref(aOptionName, false);
    }
    else
    {
      // option is not set, toggle it to set
      options.currvalues[aOptionName] = true;
      options.preferences.setPref(aOptionName, true);
    }
  }

  return value;
}

function optionsInit() {

  // hash containing the set options
  options.currvalues = {strict:     '', 
                        werror:     '', 
                        atline:     '', 
                        xml:        '',
                        relimit:    '', 
                        anonfunfux: ''
  }

  // record initial values to support resetting
  // options to their initial values 
  options.initvalues  = {};

  // record values in a stack to support pushing
  // and popping options
  options.stackvalues = [];

  options.preferences = new Preferences('javascript.options.');

  for (var optionName in options.currvalues)
  {
    if (!options.preferences.getPref(optionName))
    {
      delete options.currvalues[optionName];
    }
    else
    {
      options.initvalues[optionName] = '';
    }
  }
}

optionsInit();
optionsClear();
