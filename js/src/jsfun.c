/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * ***** BEGIN LICENSE BLOCK *****
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
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
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

/*
 * JS function support.
 */
#include "jsstddef.h"
#include <string.h>
#include "jstypes.h"
#include "jsbit.h"
#include "jsutil.h" /* Added by JSIFY */
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsconfig.h"
#include "jsdbgapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsparse.h"
#include "jsscan.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"
#include "jsexn.h"

#if JS_HAS_GENERATORS
# include "jsiter.h"
#endif

/* Generic function/call/arguments tinyids -- also reflected bit numbers. */
enum {
    CALL_ARGUMENTS  = -1,       /* predefined arguments local variable */
    ARGS_LENGTH     = -2,       /* number of actual args, arity if inactive */
    ARGS_CALLEE     = -3,       /* reference from arguments to active funobj */
    FUN_ARITY       = -4,       /* number of formal parameters; desired argc */
    FUN_NAME        = -5,       /* function name, "" if anonymous */
    FUN_CALLER      = -6        /* Function.prototype.caller, backward compat */
};

#if JSFRAME_OVERRIDE_BITS < 8
# error "not enough override bits in JSStackFrame.flags!"
#endif

#define TEST_OVERRIDE_BIT(fp, tinyid) \
    ((fp)->flags & JS_BIT(JSFRAME_OVERRIDE_SHIFT - ((tinyid) + 1)))

#define SET_OVERRIDE_BIT(fp, tinyid) \
    ((fp)->flags |= JS_BIT(JSFRAME_OVERRIDE_SHIFT - ((tinyid) + 1)))

JSBool
js_GetArgsValue(JSContext *cx, JSStackFrame *fp, jsval *vp)
{
    JSObject *argsobj;

    if (TEST_OVERRIDE_BIT(fp, CALL_ARGUMENTS)) {
        JS_ASSERT(fp->callobj);
        return OBJ_GET_PROPERTY(cx, fp->callobj,
                                ATOM_TO_JSID(cx->runtime->atomState
                                             .argumentsAtom),
                                vp);
    }
    argsobj = js_GetArgsObject(cx, fp);
    if (!argsobj)
        return JS_FALSE;
    *vp = OBJECT_TO_JSVAL(argsobj);
    return JS_TRUE;
}

static JSBool
MarkArgDeleted(JSContext *cx, JSStackFrame *fp, uintN slot)
{
    JSObject *argsobj;
    jsval bmapval, bmapint;
    size_t nbits, nbytes;
    jsbitmap *bitmap;

    argsobj = fp->argsobj;
    (void) JS_GetReservedSlot(cx, argsobj, 0, &bmapval);
    nbits = fp->argc;
    JS_ASSERT(slot < nbits);
    if (JSVAL_IS_VOID(bmapval)) {
        if (nbits <= JSVAL_INT_BITS) {
            bmapint = 0;
            bitmap = (jsbitmap *) &bmapint;
        } else {
            nbytes = JS_HOWMANY(nbits, JS_BITS_PER_WORD) * sizeof(jsbitmap);
            bitmap = (jsbitmap *) JS_malloc(cx, nbytes);
            if (!bitmap)
                return JS_FALSE;
            memset(bitmap, 0, nbytes);
            bmapval = PRIVATE_TO_JSVAL(bitmap);
            JS_SetReservedSlot(cx, argsobj, 0, bmapval);
        }
    } else {
        if (nbits <= JSVAL_INT_BITS) {
            bmapint = JSVAL_TO_INT(bmapval);
            bitmap = (jsbitmap *) &bmapint;
        } else {
            bitmap = (jsbitmap *) JSVAL_TO_PRIVATE(bmapval);
        }
    }
    JS_SET_BIT(bitmap, slot);
    if (bitmap == (jsbitmap *) &bmapint) {
        bmapval = INT_TO_JSVAL(bmapint);
        JS_SetReservedSlot(cx, argsobj, 0, bmapval);
    }
    return JS_TRUE;
}

/* NB: Infallible predicate, false does not mean error/exception. */
static JSBool
ArgWasDeleted(JSContext *cx, JSStackFrame *fp, uintN slot)
{
    JSObject *argsobj;
    jsval bmapval, bmapint;
    jsbitmap *bitmap;

    argsobj = fp->argsobj;
    (void) JS_GetReservedSlot(cx, argsobj, 0, &bmapval);
    if (JSVAL_IS_VOID(bmapval))
        return JS_FALSE;
    if (fp->argc <= JSVAL_INT_BITS) {
        bmapint = JSVAL_TO_INT(bmapval);
        bitmap = (jsbitmap *) &bmapint;
    } else {
        bitmap = (jsbitmap *) JSVAL_TO_PRIVATE(bmapval);
    }
    return JS_TEST_BIT(bitmap, slot) != 0;
}

JSBool
js_GetArgsProperty(JSContext *cx, JSStackFrame *fp, jsid id, jsval *vp)
{
    jsval val;
    JSObject *obj;
    uintN slot;

    if (TEST_OVERRIDE_BIT(fp, CALL_ARGUMENTS)) {
        JS_ASSERT(fp->callobj);
        if (!OBJ_GET_PROPERTY(cx, fp->callobj,
                              ATOM_TO_JSID(cx->runtime->atomState
                                           .argumentsAtom),
                              &val)) {
            return JS_FALSE;
        }
        if (JSVAL_IS_PRIMITIVE(val)) {
            obj = js_ValueToNonNullObject(cx, val);
            if (!obj)
                return JS_FALSE;
        } else {
            obj = JSVAL_TO_OBJECT(val);
        }
        return OBJ_GET_PROPERTY(cx, obj, id, vp);
    }

    *vp = JSVAL_VOID;
    if (JSID_IS_INT(id)) {
        slot = (uintN) JSID_TO_INT(id);
        if (slot < fp->argc) {
            if (fp->argsobj && ArgWasDeleted(cx, fp, slot))
                return OBJ_GET_PROPERTY(cx, fp->argsobj, id, vp);
            *vp = fp->argv[slot];
        } else {
            /*
             * Per ECMA-262 Ed. 3, 10.1.8, last bulleted item, do not share
             * storage between the formal parameter and arguments[k] for all
             * fp->argc <= k && k < fp->fun->nargs.  For example, in
             *
             *   function f(x) { x = 42; return arguments[0]; }
             *   f();
             *
             * the call to f should return undefined, not 42.  If fp->argsobj
             * is null at this point, as it would be in the example, return
             * undefined in *vp.
             */
            if (fp->argsobj)
                return OBJ_GET_PROPERTY(cx, fp->argsobj, id, vp);
        }
    } else {
        if (id == ATOM_TO_JSID(cx->runtime->atomState.lengthAtom)) {
            if (fp->argsobj && TEST_OVERRIDE_BIT(fp, ARGS_LENGTH))
                return OBJ_GET_PROPERTY(cx, fp->argsobj, id, vp);
            *vp = INT_TO_JSVAL((jsint) fp->argc);
        }
    }
    return JS_TRUE;
}

JSObject *
js_GetArgsObject(JSContext *cx, JSStackFrame *fp)
{
    JSObject *argsobj, *global, *parent;

    /*
     * We must be in a function activation; the function must be lightweight
     * or else fp must have a variable object.
     */
    JS_ASSERT(fp->fun && (!(fp->fun->flags & JSFUN_HEAVYWEIGHT) || fp->varobj));

    /* Skip eval and debugger frames. */
    while (fp->flags & JSFRAME_SPECIAL)
        fp = fp->down;

    /* Create an arguments object for fp only if it lacks one. */
    argsobj = fp->argsobj;
    if (argsobj)
        return argsobj;

    /* Link the new object to fp so it can get actual argument values. */
    argsobj = js_NewObject(cx, &js_ArgumentsClass, NULL, NULL);
    if (!argsobj || !JS_SetPrivate(cx, argsobj, fp)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return NULL;
    }

    /*
     * Give arguments an intrinsic scope chain link to fp's global object.
     * Since the arguments object lacks a prototype because js_ArgumentsClass
     * is not initialized, js_NewObject won't assign a default parent to it.
     *
     * Therefore if arguments is used as the head of an eval scope chain (via
     * a direct or indirect call to eval(program, arguments)), any reference
     * to a standard class object in the program will fail to resolve due to
     * js_GetClassPrototype not being able to find a global object containing
     * the standard prototype by starting from arguments and following parent.
     */
    global = fp->scopeChain;
    while ((parent = OBJ_GET_PARENT(cx, global)) != NULL)
        global = parent;
    STOBJ_SET_PARENT(argsobj, global);
    fp->argsobj = argsobj;
    return argsobj;
}

static JSBool
args_enumerate(JSContext *cx, JSObject *obj);

JSBool
js_PutArgsObject(JSContext *cx, JSStackFrame *fp)
{
    JSObject *argsobj;
    jsval bmapval, rval;
    JSBool ok;
    JSRuntime *rt;

    /*
     * Reuse args_enumerate here to reflect fp's actual arguments as indexed
     * elements of argsobj.  Do this first, before clearing and freeing the
     * deleted argument slot bitmap, because args_enumerate depends on that.
     */
    argsobj = fp->argsobj;
    ok = args_enumerate(cx, argsobj);

    /*
     * Now clear the deleted argument number bitmap slot and free the bitmap,
     * if one was actually created due to 'delete arguments[0]' or similar.
     */
    (void) JS_GetReservedSlot(cx, argsobj, 0, &bmapval);
    if (!JSVAL_IS_VOID(bmapval)) {
        JS_SetReservedSlot(cx, argsobj, 0, JSVAL_VOID);
        if (fp->argc > JSVAL_INT_BITS)
            JS_free(cx, JSVAL_TO_PRIVATE(bmapval));
    }

    /*
     * Now get the prototype properties so we snapshot fp->fun and fp->argc
     * before fp goes away.
     */
    rt = cx->runtime;
    ok &= js_GetProperty(cx, argsobj, ATOM_TO_JSID(rt->atomState.calleeAtom),
                         &rval);
    ok &= js_SetProperty(cx, argsobj, ATOM_TO_JSID(rt->atomState.calleeAtom),
                         &rval);
    ok &= js_GetProperty(cx, argsobj, ATOM_TO_JSID(rt->atomState.lengthAtom),
                         &rval);
    ok &= js_SetProperty(cx, argsobj, ATOM_TO_JSID(rt->atomState.lengthAtom),
                         &rval);

    /*
     * Clear the private pointer to fp, which is about to go away (js_Invoke).
     * Do this last because the args_enumerate and js_GetProperty calls above
     * need to follow the private slot to find fp.
     */
    ok &= JS_SetPrivate(cx, argsobj, NULL);
    fp->argsobj = NULL;
    return ok;
}

static JSBool
args_delProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    jsint slot;
    JSStackFrame *fp;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case ARGS_CALLEE:
      case ARGS_LENGTH:
        SET_OVERRIDE_BIT(fp, slot);
        break;

      default:
        if ((uintN)slot < fp->argc && !MarkArgDeleted(cx, fp, slot))
            return JS_FALSE;
        break;
    }
    return JS_TRUE;
}

static JSBool
args_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    jsint slot;
    JSStackFrame *fp;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case ARGS_CALLEE:
        if (!TEST_OVERRIDE_BIT(fp, slot))
            *vp = OBJECT_TO_JSVAL(fp->callee);
        break;

      case ARGS_LENGTH:
        if (!TEST_OVERRIDE_BIT(fp, slot))
            *vp = INT_TO_JSVAL((jsint)fp->argc);
        break;

      default:
        if ((uintN)slot < fp->argc && !ArgWasDeleted(cx, fp, slot))
            *vp = fp->argv[slot];
        break;
    }
    return JS_TRUE;
}

static JSBool
args_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;
    jsint slot;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case ARGS_CALLEE:
      case ARGS_LENGTH:
        SET_OVERRIDE_BIT(fp, slot);
        break;

      default:
        if (FUN_INTERPRETED(fp->fun) &&
            (uintN)slot < fp->argc &&
            !ArgWasDeleted(cx, fp, slot)) {
            fp->argv[slot] = *vp;
        }
        break;
    }
    return JS_TRUE;
}

static JSBool
args_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
             JSObject **objp)
{
    JSStackFrame *fp;
    uintN slot;
    JSString *str;
    JSAtom *atom;
    intN tinyid;
    jsval value;

    *objp = NULL;
    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    if (JSVAL_IS_INT(id)) {
        slot = JSVAL_TO_INT(id);
        if (slot < fp->argc && !ArgWasDeleted(cx, fp, slot)) {
            /* XXX ECMA specs DontEnum, contrary to other array-like objects */
            if (!js_DefineProperty(cx, obj, INT_JSVAL_TO_JSID(id),
                                   fp->argv[slot],
                                   args_getProperty, args_setProperty,
                                   0, NULL)) {
                return JS_FALSE;
            }
            *objp = obj;
        }
    } else {
        str = JSVAL_TO_STRING(id);
        atom = cx->runtime->atomState.lengthAtom;
        if (str == ATOM_TO_STRING(atom)) {
            tinyid = ARGS_LENGTH;
            value = INT_TO_JSVAL(fp->argc);
        } else {
            atom = cx->runtime->atomState.calleeAtom;
            if (str == ATOM_TO_STRING(atom)) {
                tinyid = ARGS_CALLEE;
                value = OBJECT_TO_JSVAL(fp->callee);
            } else {
                atom = NULL;

                /* Quell GCC overwarnings. */
                tinyid = 0;
                value = JSVAL_NULL;
            }
        }

        if (atom && !TEST_OVERRIDE_BIT(fp, tinyid)) {
            if (!js_DefineNativeProperty(cx, obj, ATOM_TO_JSID(atom), value,
                                         args_getProperty, args_setProperty, 0,
                                         SPROP_HAS_SHORTID, tinyid, NULL)) {
                return JS_FALSE;
            }
            *objp = obj;
        }
    }

    return JS_TRUE;
}

static JSBool
args_enumerate(JSContext *cx, JSObject *obj)
{
    JSStackFrame *fp;
    JSObject *pobj;
    JSProperty *prop;
    uintN slot, argc;

    fp = (JSStackFrame *)
         JS_GetInstancePrivate(cx, obj, &js_ArgumentsClass, NULL);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->argsobj);

    /*
     * Trigger reflection with value snapshot in args_resolve using a series
     * of js_LookupProperty calls.  We handle length, callee, and the indexed
     * argument properties.  We know that args_resolve covers all these cases
     * and creates direct properties of obj, but that it may fail to resolve
     * length or callee if overridden.
     */
    if (!js_LookupProperty(cx, obj,
                           ATOM_TO_JSID(cx->runtime->atomState.lengthAtom),
                           &pobj, &prop)) {
        return JS_FALSE;
    }
    if (prop)
        OBJ_DROP_PROPERTY(cx, pobj, prop);

    if (!js_LookupProperty(cx, obj,
                           ATOM_TO_JSID(cx->runtime->atomState.calleeAtom),
                           &pobj, &prop)) {
        return JS_FALSE;
    }
    if (prop)
        OBJ_DROP_PROPERTY(cx, pobj, prop);

    argc = fp->argc;
    for (slot = 0; slot < argc; slot++) {
        if (!js_LookupProperty(cx, obj, INT_TO_JSID((jsint)slot), &pobj, &prop))
            return JS_FALSE;
        if (prop)
            OBJ_DROP_PROPERTY(cx, pobj, prop);
    }
    return JS_TRUE;
}

#if JS_HAS_GENERATORS
/*
 * If a generator-iterator's arguments or call object escapes, it needs to
 * mark its generator object.
 */
static void
args_or_call_trace(JSTracer *trc, JSObject *obj)
{
    JSStackFrame *fp;

    fp = (JSStackFrame *) JS_GetPrivate(trc->context, obj);
    if (fp && (fp->flags & JSFRAME_GENERATOR)) {
        JS_CALL_OBJECT_TRACER(trc, FRAME_TO_GENERATOR(fp)->obj,
                              "FRAME_TO_GENERATOR(fp)->obj");
    }
}
#else
# define args_or_call_trace NULL
#endif

/*
 * The Arguments class is not initialized via JS_InitClass, and must not be,
 * because its name is "Object".  Per ECMA, that causes instances of it to
 * delegate to the object named by Object.prototype.  It also ensures that
 * arguments.toString() returns "[object Object]".
 *
 * The JSClass functions below collaborate to lazily reflect and synchronize
 * actual argument values, argument count, and callee function object stored
 * in a JSStackFrame with their corresponding property values in the frame's
 * arguments object.
 */
JSClass js_ArgumentsClass = {
    js_Object_str,
    JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_HAS_RESERVED_SLOTS(1) |
    JSCLASS_MARK_IS_TRACE | JSCLASS_HAS_CACHED_PROTO(JSProto_Object),
    JS_PropertyStub,    args_delProperty,
    args_getProperty,   args_setProperty,
    args_enumerate,     (JSResolveOp) args_resolve,
    JS_ConvertStub,     JS_FinalizeStub,
    NULL,               NULL,
    NULL,               NULL,
    NULL,               NULL,
    JS_CLASS_TRACE(args_or_call_trace), NULL
};

JSObject *
js_GetCallObject(JSContext *cx, JSStackFrame *fp, JSObject *parent)
{
    JSObject *callobj, *funobj;

    /* Create a call object for fp only if it lacks one. */
    JS_ASSERT(fp->fun);
    callobj = fp->callobj;
    if (callobj)
        return callobj;
    JS_ASSERT(fp->fun);

    /* The default call parent is its function's parent (static link). */
    if (!parent) {
        funobj = fp->callee;
        if (funobj)
            parent = OBJ_GET_PARENT(cx, funobj);
    }

    /* Create the call object and link it to its stack frame. */
    callobj = js_NewObject(cx, &js_CallClass, NULL, parent);
    if (!callobj || !JS_SetPrivate(cx, callobj, fp)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return NULL;
    }
    fp->callobj = callobj;

    /* Make callobj be the scope chain and the variables object. */
    JS_ASSERT(fp->scopeChain == parent);
    fp->scopeChain = callobj;
    fp->varobj = callobj;
    return callobj;
}

static JSBool
call_enumerate(JSContext *cx, JSObject *obj);

JSBool
js_PutCallObject(JSContext *cx, JSStackFrame *fp)
{
    JSObject *callobj;
    JSBool ok;
    jsid argsid;
    jsval aval;

    /*
     * Reuse call_enumerate here to reflect all actual args and vars into the
     * call object from fp.
     */
    callobj = fp->callobj;
    if (!callobj)
        return JS_TRUE;
    ok = call_enumerate(cx, callobj);

    /*
     * Get the arguments object to snapshot fp's actual argument values.
     */
    if (fp->argsobj) {
        if (!TEST_OVERRIDE_BIT(fp, CALL_ARGUMENTS)) {
            argsid = ATOM_TO_JSID(cx->runtime->atomState.argumentsAtom);
            aval = OBJECT_TO_JSVAL(fp->argsobj);
            ok &= js_SetProperty(cx, callobj, argsid, &aval);
        }
        ok &= js_PutArgsObject(cx, fp);
    }

    /*
     * Clear the private pointer to fp, which is about to go away (js_Invoke).
     * Do this last because the call_enumerate and js_GetProperty calls above
     * need to follow the private slot to find fp.
     */
    ok &= JS_SetPrivate(cx, callobj, NULL);
    fp->callobj = NULL;
    return ok;
}

static JSBool
call_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;
    jsint slot;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->fun);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case CALL_ARGUMENTS:
        if (!TEST_OVERRIDE_BIT(fp, slot)) {
            JSObject *argsobj = js_GetArgsObject(cx, fp);
            if (!argsobj)
                return JS_FALSE;
            *vp = OBJECT_TO_JSVAL(argsobj);
        }
        break;

      default:
        if ((uintN)slot < JS_MAX(fp->argc, fp->fun->nargs))
            *vp = fp->argv[slot];
        break;
    }
    return JS_TRUE;
}

static JSBool
call_setProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;
    jsint slot;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->fun);

    slot = JSVAL_TO_INT(id);
    switch (slot) {
      case CALL_ARGUMENTS:
        SET_OVERRIDE_BIT(fp, slot);
        break;

      default:
        if ((uintN)slot < JS_MAX(fp->argc, fp->fun->nargs))
            fp->argv[slot] = *vp;
        break;
    }
    return JS_TRUE;
}

JSBool
js_GetCallVariable(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;

    JS_ASSERT(JSVAL_IS_INT(id));
    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (fp) {
        JS_ASSERT((uintN) JSVAL_TO_INT(id) < fp->nvars);
        *vp = fp->vars[JSVAL_TO_INT(id)];
    }
    return JS_TRUE;
}

JSBool
js_SetCallVariable(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    JSStackFrame *fp;

    JS_ASSERT(JSVAL_IS_INT(id));
    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (fp) {
        JS_ASSERT((uintN) JSVAL_TO_INT(id) < fp->nvars);
        fp->vars[JSVAL_TO_INT(id)] = *vp;
    }
    return JS_TRUE;
}

static JSBool
call_enumerate(JSContext *cx, JSObject *obj)
{
    JSStackFrame *fp;
    JSObject *funobj, *pobj;
    JSScope *scope;
    JSScopeProperty *sprop, *cprop;
    jsval *vec;
    jsid id;
    JSProperty *prop;

    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;

    /*
     * Do not enumerate a cloned function object at fp->callee, it may have
     * gained its own (mutable) scope (e.g., a brutally-shared XUL script sets
     * the clone's prototype property).  We must enumerate the function object
     * that was decorated with parameter and local variable properties by the
     * compiler when the compiler created fp->fun, namely fp->fun->object.
     *
     * Contrast with call_resolve, where we prefer fp->callee, because we'll
     * use js_LookupProperty to find any overridden properties in that object,
     * if it was a mutated clone; and if not, we will search its prototype,
     * fp->fun->object, to find compiler-created params and locals.
     */
    funobj = fp->fun->object;
    if (!funobj)
        return JS_TRUE;

    /*
     * Reflect actual args from fp->argv for formal parameters, and local vars
     * and functions in fp->vars for declared variables and nested-at-top-level
     * local functions.
     */
    scope = OBJ_SCOPE(funobj);
    for (sprop = SCOPE_LAST_PROP(scope); sprop; sprop = sprop->parent) {
        if (!JSID_IS_HIDDEN(sprop->id))
            continue;
        vec = (sprop->getter == JS_HIDDEN_ARG_GETTER) ? fp->argv : fp->vars;

        /* Trigger reflection by looking up the unhidden atom for sprop->id. */
        id = JSID_UNHIDE_NAME(sprop->id);
        if (!js_LookupProperty(cx, obj, id, &pobj, &prop))
            return JS_FALSE;

        /*
         * If we found the property in a different object, don't try sticking
         * it into wrong slots vector. This can occur because we have a mutable
         * __proto__ slot, and cloned function objects rely on their __proto__
         * to delegate to the object that contains the var and arg properties.
         */
        if (!prop || pobj != obj) {
            if (prop)
                OBJ_DROP_PROPERTY(cx, pobj, prop);
            continue;
        }
        cprop = (JSScopeProperty *)prop;
        LOCKED_OBJ_SET_SLOT(obj, cprop->slot, vec[(uint16) sprop->shortid]);
        OBJ_DROP_PROPERTY(cx, obj, prop);
    }

    return JS_TRUE;
}

static JSBool
call_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
             JSObject **objp)
{
    JSStackFrame *fp;
    JSString *str;
    JSAtom *atom;
    JSLocalKind localKind;
    JSPropertyOp getter, setter;
    uintN slot, attrs, spflags;
    jsval *vp;
    int shortid;

    fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
    if (!fp)
        return JS_TRUE;
    JS_ASSERT(fp->fun);

    if (!JSVAL_IS_STRING(id))
        return JS_TRUE;

    if (!fp->callee)
        return JS_TRUE;
    JS_ASSERT(GET_FUNCTION_PRIVATE(cx, fp->callee) == fp->fun);

    str = JSVAL_TO_STRING(id);
    atom = js_AtomizeString(cx, str, 0);
    if (!atom)
        return JS_FALSE;

    localKind = js_LookupLocal(cx, fp->fun, atom, &slot);
    if (localKind != JSLOCAL_NONE) {
        if (localKind == JSLOCAL_ARG) {
            JS_ASSERT(slot < fp->fun->nargs);
            vp = fp->argv;
            getter = setter = NULL;
            attrs = JSPROP_PERMANENT;
            spflags = 0;
            shortid = 0;
        } else {
            JS_ASSERT(localKind == JSLOCAL_VAR || localKind == JSLOCAL_CONST);
            JS_ASSERT(fp->fun->u.i.nvars == fp->nvars);
            JS_ASSERT(slot < fp->nvars);
            vp = fp->vars;
            getter = js_GetCallVariable;
            setter = js_SetCallVariable;
            attrs = (localKind == JSLOCAL_CONST)
                    ? JSPROP_PERMANENT | JSPROP_READONLY
                    : JSPROP_PERMANENT;
            spflags = SPROP_HAS_SHORTID;
            shortid = (int) slot;
        }
        if (!js_DefineNativeProperty(cx, obj, ATOM_TO_JSID(atom), vp[slot],
                                     getter, setter, attrs,
                                     spflags, shortid, NULL)) {
            return JS_FALSE;
        }
        *objp = obj;
        return JS_TRUE;
    }

    if (!(flags & JSRESOLVE_ASSIGNING)) {
        /*
         * Resolve arguments so that we never store a particular Call object's
         * arguments object reference in a Call prototype's |arguments| slot.
         */
        atom = cx->runtime->atomState.argumentsAtom;
        if (id == ATOM_KEY(atom)) {
            if (!js_DefineNativeProperty(cx, obj,
                                         ATOM_TO_JSID(atom), JSVAL_VOID,
                                         NULL, NULL, JSPROP_PERMANENT,
                                         SPROP_HAS_SHORTID, CALL_ARGUMENTS,
                                         NULL)) {
                return JS_FALSE;
            }
            *objp = obj;
            return JS_TRUE;
        }
    }
    return JS_TRUE;
}

static JSBool
call_convert(JSContext *cx, JSObject *obj, JSType type, jsval *vp)
{
    JSStackFrame *fp;

    if (type == JSTYPE_FUNCTION) {
        fp = (JSStackFrame *) JS_GetPrivate(cx, obj);
        if (fp) {
            JS_ASSERT(fp->fun);
            *vp = OBJECT_TO_JSVAL(fp->callee);
        }
    }
    return JS_TRUE;
}

JSClass js_CallClass = {
    js_Call_str,
    JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_IS_ANONYMOUS |
    JSCLASS_MARK_IS_TRACE | JSCLASS_HAS_CACHED_PROTO(JSProto_Call),
    JS_PropertyStub,    JS_PropertyStub,
    call_getProperty,   call_setProperty,
    call_enumerate,     (JSResolveOp)call_resolve,
    call_convert,       JS_FinalizeStub,
    NULL,               NULL,
    NULL,               NULL,
    NULL,               NULL,
    JS_CLASS_TRACE(args_or_call_trace), NULL,
};

/*
 * ECMA-262 specifies that length is a property of function object instances,
 * but we can avoid that space cost by delegating to a prototype property that
 * is JSPROP_PERMANENT and JSPROP_SHARED.  Each fun_getProperty call computes
 * a fresh length value based on the arity of the individual function object's
 * private data.
 *
 * The extensions below other than length, i.e., the ones not in ECMA-262,
 * are neither JSPROP_READONLY nor JSPROP_SHARED, because for compatibility
 * with ECMA we must allow a delegating object to override them. Therefore to
 * avoid entraining garbage in Function.prototype slots, they must be resolved
 * in non-prototype function objects, wherefore the lazy_function_props table
 * and fun_resolve's use of it.
 */
#define LENGTH_PROP_ATTRS (JSPROP_READONLY|JSPROP_PERMANENT|JSPROP_SHARED)

static JSPropertySpec function_props[] = {
    {js_length_str,    ARGS_LENGTH,    LENGTH_PROP_ATTRS, 0,0},
    {0,0,0,0,0}
};

typedef struct LazyFunctionProp {
    uint16      atomOffset;
    int8        tinyid;
    uint8       attrs;
} LazyFunctionProp;

/* NB: no sentinel at the end -- use JS_ARRAY_LENGTH to bound loops. */
static LazyFunctionProp lazy_function_props[] = {
    {ATOM_OFFSET(arguments), CALL_ARGUMENTS, JSPROP_PERMANENT},
    {ATOM_OFFSET(arity),     FUN_ARITY,      JSPROP_PERMANENT},
    {ATOM_OFFSET(caller),    FUN_CALLER,     JSPROP_PERMANENT},
    {ATOM_OFFSET(name),      FUN_NAME,       JSPROP_PERMANENT},
};

static JSBool
fun_getProperty(JSContext *cx, JSObject *obj, jsval id, jsval *vp)
{
    jsint slot;
    JSFunction *fun;
    JSStackFrame *fp;

    if (!JSVAL_IS_INT(id))
        return JS_TRUE;
    slot = JSVAL_TO_INT(id);

    /*
     * Loop because getter and setter can be delegated from another class,
     * but loop only for ARGS_LENGTH because we must pretend that f.length
     * is in each function instance f, per ECMA-262, instead of only in the
     * Function.prototype object (we use JSPROP_PERMANENT with JSPROP_SHARED
     * to make it appear so).
     *
     * This code couples tightly to the attributes for the function_props[]
     * initializers above, and to js_SetProperty and js_HasOwnPropertyHelper.
     *
     * It's important to allow delegating objects, even though they inherit
     * this getter (fun_getProperty), to override arguments, arity, caller,
     * and name.  If we didn't return early for slot != ARGS_LENGTH, we would
     * clobber *vp with the native property value, instead of letting script
     * override that value in delegating objects.
     *
     * Note how that clobbering is what simulates JSPROP_READONLY for all of
     * the non-standard properties when the directly addressed object (obj)
     * is a function object (i.e., when this loop does not iterate).
     */
    while (!(fun = (JSFunction *)
                   JS_GetInstancePrivate(cx, obj, &js_FunctionClass, NULL))) {
        if (slot != ARGS_LENGTH)
            return JS_TRUE;
        obj = OBJ_GET_PROTO(cx, obj);
        if (!obj)
            return JS_TRUE;
    }

    /* Find fun's top-most activation record. */
    for (fp = cx->fp; fp && (fp->fun != fun || (fp->flags & JSFRAME_SPECIAL));
         fp = fp->down) {
        continue;
    }

    switch (slot) {
      case CALL_ARGUMENTS:
        /* Warn if strict about f.arguments or equivalent unqualified uses. */
        if (!JS_ReportErrorFlagsAndNumber(cx,
                                          JSREPORT_WARNING | JSREPORT_STRICT,
                                          js_GetErrorMessage, NULL,
                                          JSMSG_DEPRECATED_USAGE,
                                          js_arguments_str)) {
            return JS_FALSE;
        }
        if (fp) {
            if (!js_GetArgsValue(cx, fp, vp))
                return JS_FALSE;
        } else {
            *vp = JSVAL_NULL;
        }
        break;

      case ARGS_LENGTH:
      case FUN_ARITY:
            *vp = INT_TO_JSVAL((jsint)fun->nargs);
        break;

      case FUN_NAME:
        *vp = fun->atom
              ? ATOM_KEY(fun->atom)
              : STRING_TO_JSVAL(cx->runtime->emptyString);
        break;

      case FUN_CALLER:
        if (fp && fp->down && fp->down->fun)
            *vp = OBJECT_TO_JSVAL(fp->down->callee);
        else
            *vp = JSVAL_NULL;
        if (!JSVAL_IS_PRIMITIVE(*vp) && cx->runtime->checkObjectAccess) {
            id = ATOM_KEY(cx->runtime->atomState.callerAtom);
            if (!cx->runtime->checkObjectAccess(cx, obj, id, JSACC_READ, vp))
                return JS_FALSE;
        }
        break;

      default:
        /* XXX fun[0] and fun.arguments[0] are equivalent. */
        if (fp && fp->fun && (uintN)slot < fp->fun->nargs)
            *vp = fp->argv[slot];
        break;
    }

    return JS_TRUE;
}

static JSBool
fun_enumerate(JSContext *cx, JSObject *obj)
{
    jsid prototypeId;
    JSObject *pobj;
    JSProperty *prop;

    prototypeId = ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom);
    if (!OBJ_LOOKUP_PROPERTY(cx, obj, prototypeId, &pobj, &prop))
        return JS_FALSE;
    if (prop)
        OBJ_DROP_PROPERTY(cx, pobj, prop);
    return JS_TRUE;
}

static JSBool
fun_resolve(JSContext *cx, JSObject *obj, jsval id, uintN flags,
            JSObject **objp)
{
    JSFunction *fun;
    JSAtom *atom;
    uintN i;

    if (!JSVAL_IS_STRING(id))
        return JS_TRUE;

    fun = GET_FUNCTION_PRIVATE(cx, obj);
    JS_ASSERT(fun->object);

    /*
     * No need to reflect fun.prototype in 'fun.prototype = ... '.
     *
     * This is not just an optimization, because we must not resolve when
     * defining hidden properties during compilation. The setup code for the
     * prototype and the lazy properties below eventually calls the property
     * hooks for the function object. That in turn calls fun_reserveSlots to
     * get the number of the reserved slots which is just the number of
     * regular expressions literals in the function. When compiling, that
     * number is not yet ready so we must make sure that fun_resolve does
     * nothing until the code for the function is generated.
     */
    if (flags & JSRESOLVE_ASSIGNING)
        return JS_TRUE;

    /*
     * Ok, check whether id is 'prototype' and bootstrap the function object's
     * prototype property.
     */
    atom = cx->runtime->atomState.classPrototypeAtom;
    if (id == ATOM_KEY(atom)) {
        JSObject *proto, *parentProto;
        jsval pval;

        proto = parentProto = NULL;
        if (fun->object != obj &&
            (!cx->runtime->findObjectPrincipals ||
             cx->runtime->findObjectPrincipals(cx, obj) ==
             cx->runtime->findObjectPrincipals(cx, fun->object))) {
            /*
             * Clone of a function where the clone and the object owning fun
             * appear to be in the same trust domain: make the cloned function
             * object's 'prototype' property value have the same class as the
             * clone-parent's 'prototype' value.
             */
            if (!OBJ_GET_PROPERTY(cx, fun->object, ATOM_TO_JSID(atom), &pval))
                return JS_FALSE;
            if (!JSVAL_IS_PRIMITIVE(pval)) {
                /*
                 * We are about to allocate a new object, so hack the newborn
                 * root until then to protect pval in case it is figuratively
                 * up in the air, with no strong refs protecting it.
                 */
                cx->weakRoots.newborn[GCX_OBJECT] = JSVAL_TO_GCTHING(pval);
                parentProto = JSVAL_TO_OBJECT(pval);
            }
        }

        /*
         * Beware of the wacky case of a user function named Object -- trying
         * to find a prototype for that will recur back here _ad perniciem_.
         */
        if (!parentProto && fun->atom == CLASS_ATOM(cx, Object))
            return JS_TRUE;

        /*
         * If resolving "prototype" in a clone, clone the parent's prototype.
         * Pass the constructor's (obj's) parent as the prototype parent, to
         * avoid defaulting to parentProto.constructor.__parent__.
         */
        proto = js_NewObject(cx, &js_ObjectClass, parentProto,
                             OBJ_GET_PARENT(cx, obj));
        if (!proto)
            return JS_FALSE;

        /*
         * ECMA (15.3.5.2) says that constructor.prototype is DontDelete for
         * user-defined functions, but DontEnum | ReadOnly | DontDelete for
         * native "system" constructors such as Object or Function.  So lazily
         * set the former here in fun_resolve, but eagerly define the latter
         * in JS_InitClass, with the right attributes.
         */
        if (!js_SetClassPrototype(cx, obj, proto,
                                  JSPROP_ENUMERATE | JSPROP_PERMANENT)) {
            cx->weakRoots.newborn[GCX_OBJECT] = NULL;
            return JS_FALSE;
        }
        *objp = obj;
        return JS_TRUE;
    }

    for (i = 0; i < JS_ARRAY_LENGTH(lazy_function_props); i++) {
        LazyFunctionProp *lfp = &lazy_function_props[i];

        atom = OFFSET_TO_ATOM(cx->runtime, lfp->atomOffset);
        if (id == ATOM_KEY(atom)) {
            if (!js_DefineNativeProperty(cx, obj,
                                         ATOM_TO_JSID(atom), JSVAL_VOID,
                                         NULL, NULL, lfp->attrs,
                                         SPROP_HAS_SHORTID, lfp->tinyid,
                                         NULL)) {
                return JS_FALSE;
            }
            *objp = obj;
            return JS_TRUE;
        }
    }

    return JS_TRUE;
}

static JSBool
fun_convert(JSContext *cx, JSObject *obj, JSType type, jsval *vp)
{
    switch (type) {
      case JSTYPE_FUNCTION:
        *vp = OBJECT_TO_JSVAL(obj);
        return JS_TRUE;
      default:
        return js_TryValueOf(cx, obj, type, vp);
    }
}

#if JS_HAS_XDR

#include "jsxdrapi.h"

/* XXX store parent and proto, if defined */
static JSBool
fun_xdrObject(JSXDRState *xdr, JSObject **objp)
{
    JSContext *cx;
    JSFunction *fun;
    uint32 nullAtom;            /* flag to indicate if fun->atom is NULL */
    uintN nargs, nvars, n;
    uint32 localsword;          /* word to xdr argument and variable counts */
    uint32 flagsword;           /* originally only flags was JS_XDRUint8'd */
    JSTempValueRooter tvr;
    JSBool ok;

    cx = xdr->cx;
    if (xdr->mode == JSXDR_ENCODE) {
        fun = GET_FUNCTION_PRIVATE(cx, *objp);
        if (!FUN_INTERPRETED(fun)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_NOT_SCRIPTED_FUNCTION,
                                 JS_GetFunctionName(fun));
            return JS_FALSE;
        }
        nullAtom = !fun->atom;
        nargs = fun->nargs;
        nvars = fun->u.i.nvars;
        localsword = (nargs << 16) | nvars;
        flagsword = fun->flags;
    } else {
        fun = js_NewFunction(cx, NULL, NULL, 0, JSFUN_INTERPRETED, NULL, NULL);
        if (!fun)
            return JS_FALSE;
#ifdef __GNUC__
        nvars = nargs = 0;   /* quell GCC uninitialized warning */
#endif
    }

    /* From here on, control flow must flow through label out. */
    JS_PUSH_TEMP_ROOT_OBJECT(cx, fun->object, &tvr);
    ok = JS_TRUE;

    if (!JS_XDRUint32(xdr, &nullAtom))
        goto bad;
    if (!nullAtom && !js_XDRStringAtom(xdr, &fun->atom))
        goto bad;
    if (!JS_XDRUint32(xdr, &localsword) ||
        !JS_XDRUint32(xdr, &flagsword)) {
        goto bad;
    }

    if (xdr->mode == JSXDR_DECODE) {
        nargs = localsword >> 16;
        nvars = localsword & JS_BITMASK(16);
        JS_ASSERT(flagsword | JSFUN_INTERPRETED);
        fun->flags = (uint16) flagsword;
    }

    /* do arguments and local vars */
    if (fun->object && (n = nargs + nvars) != 0) {
        void *mark;
        uintN i;
        uintN bitmapLength;
        uint32 *bitmap;
        JSAtom **names, *name;
        uint32 localKind;

        mark = JS_ARENA_MARK(&xdr->cx->tempPool);

        /* From this point the control must flow through label release_mark. */
        bitmapLength = JS_HOWMANY(n, JS_BITS_PER_UINT32);
        if (xdr->mode == JSXDR_ENCODE) {
            names = js_GetLocalNames(xdr->cx, fun, &xdr->cx->tempPool, &bitmap);
            if (!names) {
                ok = JS_FALSE;
                goto release_mark;
            }
        } else {
#ifdef __GNUC__
            names = NULL;   /* quell GCC uninitialized warning */
#endif
            JS_ARENA_ALLOCATE_CAST(bitmap, uint32 *, &xdr->cx->tempPool,
                                   bitmapLength * sizeof *bitmap);
            if (!bitmap) {
                JS_ReportOutOfMemory(xdr->cx);
                ok = JS_FALSE;
                goto release_mark;
            }
        }
        for (i = 0; i != bitmapLength; ++i) {
            ok = JS_XDRUint32(xdr, &bitmap[i]);
            if (!ok)
                goto release_mark;
        }
        for (i = 0; i != n; ++i) {
            if (i < nargs &&
                !(bitmap[i / JS_BITS_PER_UINT32] &
                  JS_BIT(i & (JS_BITS_PER_UINT32 - 1)))) {
                if (xdr->mode == JSXDR_DECODE) {
                    ok = js_AddLocal(xdr->cx, fun, NULL, JSLOCAL_ARG);
                    if (!ok)
                        goto release_mark;
                } else {
                    JS_ASSERT(!names[i]);
                }
                continue;
            }
            if (xdr->mode == JSXDR_ENCODE)
                name = names[i];
            ok = js_XDRStringAtom(xdr, &name);
            if (!ok)
                goto release_mark;
            if (xdr->mode == JSXDR_DECODE) {
                localKind = (i < nargs)
                            ? JSLOCAL_ARG
                            : bitmap[i / JS_BITS_PER_UINT32] &
                              JS_BIT(i & (JS_BITS_PER_UINT32 - 1))
                            ? JSLOCAL_CONST
                            : JSLOCAL_VAR;
                ok = js_AddLocal(xdr->cx, fun, name, localKind);
                if (!ok)
                    goto release_mark;
            }
        }
        ok = JS_TRUE;

      release_mark:
        JS_ARENA_RELEASE(&xdr->cx->tempPool, mark);
        if (!ok)
            goto out;
    }

    if (!js_XDRScript(xdr, &fun->u.i.script, NULL))
        goto bad;

    if (xdr->mode == JSXDR_DECODE) {
        *objp = fun->object;
        js_CallNewScriptHook(cx, fun->u.i.script, fun);
    }

out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    return ok;

bad:
    ok = JS_FALSE;
    goto out;
}

#else  /* !JS_HAS_XDR */

#define fun_xdrObject NULL

#endif /* !JS_HAS_XDR */

/*
 * [[HasInstance]] internal method for Function objects: fetch the .prototype
 * property of its 'this' parameter, and walks the prototype chain of v (only
 * if v is an object) returning true if .prototype is found.
 */
static JSBool
fun_hasInstance(JSContext *cx, JSObject *obj, jsval v, JSBool *bp)
{
    jsval pval;

    if (!OBJ_GET_PROPERTY(cx, obj,
                          ATOM_TO_JSID(cx->runtime->atomState
                                       .classPrototypeAtom),
                          &pval)) {
        return JS_FALSE;
    }

    if (JSVAL_IS_PRIMITIVE(pval)) {
        /*
         * Throw a runtime error if instanceof is called on a function that
         * has a non-object as its .prototype value.
         */
        js_ReportValueError(cx, JSMSG_BAD_PROTOTYPE,
                            -1, OBJECT_TO_JSVAL(obj), NULL);
        return JS_FALSE;
    }

    return js_IsDelegate(cx, JSVAL_TO_OBJECT(pval), v, bp);
}

static void
fun_trace(JSTracer *trc, JSObject *obj)
{
    JSFunction *fun;

    /* A newborn function object may have a not yet initialized private slot. */
    fun = (JSFunction *) JS_GetPrivate(trc->context, obj);
    if (fun)
        JS_CALL_TRACER(trc, fun, JSTRACE_FUNCTION, "private");
}

static uint32
fun_reserveSlots(JSContext *cx, JSObject *obj)
{
    JSFunction *fun;

    /*
     * We use JS_GetPrivate and not GET_FUNCTION_PRIVATE because during
     * js_InitFunctionClass invocation the function is called before the
     * private slot of the function object is set.
     */
    fun = (JSFunction *) JS_GetPrivate(cx, obj);
    return (fun && FUN_INTERPRETED(fun) &&
            fun->u.i.script && fun->u.i.script->regexpsOffset != 0)
           ? JS_SCRIPT_REGEXPS(fun->u.i.script)->length
           : 0;
}

/*
 * Reserve two slots in all function objects for XPConnect.  Note that this
 * does not bloat every instance, only those on which reserved slots are set,
 * and those on which ad-hoc properties are defined.
 */
JS_FRIEND_DATA(JSClass) js_FunctionClass = {
    js_Function_str,
    JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE | JSCLASS_HAS_RESERVED_SLOTS(2) |
    JSCLASS_MARK_IS_TRACE | JSCLASS_HAS_CACHED_PROTO(JSProto_Function),
    JS_PropertyStub,  JS_PropertyStub,
    fun_getProperty,  JS_PropertyStub,
    fun_enumerate,    (JSResolveOp)fun_resolve,
    fun_convert,      JS_FinalizeStub,
    NULL,             NULL,
    NULL,             NULL,
    fun_xdrObject,    fun_hasInstance,
    JS_CLASS_TRACE(fun_trace), fun_reserveSlots
};

static JSBool
fun_toStringHelper(JSContext *cx, uint32 indent, uintN argc, jsval *vp)
{
    jsval fval;
    JSObject *obj;
    JSFunction *fun;
    JSString *str;

    fval = vp[1];
    if (!VALUE_IS_FUNCTION(cx, fval)) {
        /*
         * If we don't have a function to start off with, try converting the
         * object to a function. If that doesn't work, complain.
         */
        if (!JSVAL_IS_PRIMITIVE(fval)) {
            obj = JSVAL_TO_OBJECT(fval);
            if (!OBJ_GET_CLASS(cx, obj)->convert(cx, obj, JSTYPE_FUNCTION,
                                                 &fval)) {
                return JS_FALSE;
            }
            vp[1] = fval;
        }
        if (!VALUE_IS_FUNCTION(cx, fval)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_INCOMPATIBLE_PROTO,
                                 js_Function_str, js_toString_str,
                                 JS_GetTypeName(cx, JS_TypeOfValue(cx, fval)));
            return JS_FALSE;
        }
    }

    obj = JSVAL_TO_OBJECT(fval);
    if (argc != 0 && !js_ValueToECMAUint32(cx, vp[2], &indent))
        return JS_FALSE;

    JS_ASSERT(JS_ObjectIsFunction(cx, obj));
    fun = GET_FUNCTION_PRIVATE(cx, obj);
    if (!fun)
        return JS_TRUE;
    str = JS_DecompileFunction(cx, fun, (uintN)indent);
    if (!str)
        return JS_FALSE;
    *vp = STRING_TO_JSVAL(str);
    return JS_TRUE;
}

static JSBool
fun_toString(JSContext *cx, uintN argc, jsval *vp)
{
    return fun_toStringHelper(cx, 0, argc,  vp);
}

#if JS_HAS_TOSOURCE
static JSBool
fun_toSource(JSContext *cx, uintN argc, jsval *vp)
{
    return fun_toStringHelper(cx, JS_DONT_PRETTY_PRINT, argc, vp);
}
#endif

static const char call_str[] = "call";

static JSBool
fun_call(JSContext *cx, uintN argc, jsval *vp)
{
    JSObject *obj;
    jsval fval, *argv, *invokevp;
    JSString *str;
    void *mark;
    JSBool ok;

    obj = JSVAL_TO_OBJECT(vp[1]);
    if (!OBJ_DEFAULT_VALUE(cx, obj, JSTYPE_FUNCTION, &vp[1]))
        return JS_FALSE;
    fval = vp[1];

    if (!VALUE_IS_FUNCTION(cx, fval)) {
        str = JS_ValueToString(cx, fval);
        if (str) {
            const char *bytes = js_GetStringBytes(cx, str);

            if (bytes) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_INCOMPATIBLE_PROTO,
                                     js_Function_str, call_str,
                                     bytes);
            }
        }
        return JS_FALSE;
    }

    argv = vp + 2;
    if (argc == 0) {
        /* Call fun with its global object as the 'this' param if no args. */
        obj = NULL;
    } else {
        /* Otherwise convert the first arg to 'this' and skip over it. */
        if (!JSVAL_IS_PRIMITIVE(argv[0]))
            obj = JSVAL_TO_OBJECT(argv[0]);
        else if (!js_ValueToObject(cx, argv[0], &obj))
            return JS_FALSE;
        argc--;
        argv++;
    }

    /* Allocate stack space for fval, obj, and the args. */
    invokevp = js_AllocStack(cx, 2 + argc, &mark);
    if (!invokevp)
        return JS_FALSE;

    /* Push fval, obj, and the args. */
    invokevp[0] = fval;
    invokevp[1] = OBJECT_TO_JSVAL(obj);
    memcpy(invokevp + 2, argv, argc * sizeof *argv);

    ok = js_Invoke(cx, argc, invokevp, JSINVOKE_INTERNAL);
    *vp = *invokevp;
    js_FreeStack(cx, mark);
    return ok;
}

static JSBool
fun_apply(JSContext *cx, uintN argc, jsval *vp)
{
    JSObject *obj, *aobj;
    jsval fval, *invokevp, *sp;
    JSString *str;
    jsuint length;
    JSBool arraylike, ok;
    void *mark;
    uintN i;

    if (argc == 0) {
        /* Will get globalObject as 'this' and no other arguments. */
        return fun_call(cx, argc, vp);
    }

    obj = JSVAL_TO_OBJECT(vp[1]);
    if (!OBJ_DEFAULT_VALUE(cx, obj, JSTYPE_FUNCTION, &vp[1]))
        return JS_FALSE;
    fval = vp[1];

    if (!VALUE_IS_FUNCTION(cx, fval)) {
        str = JS_ValueToString(cx, fval);
        if (str) {
            const char *bytes = js_GetStringBytes(cx, str);

            if (bytes) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_INCOMPATIBLE_PROTO,
                                     js_Function_str, "apply",
                                     bytes);
            }
        }
        return JS_FALSE;
    }

    /* Quell GCC overwarnings. */
    aobj = NULL;
    length = 0;

    if (argc >= 2) {
        /* If the 2nd arg is null or void, call the function with 0 args. */
        if (JSVAL_IS_NULL(vp[3]) || JSVAL_IS_VOID(vp[3])) {
            argc = 0;
        } else {
            /* The second arg must be an array (or arguments object). */
            arraylike = JS_FALSE;
            if (!JSVAL_IS_PRIMITIVE(vp[3])) {
                aobj = JSVAL_TO_OBJECT(vp[3]);
                if (!js_IsArrayLike(cx, aobj, &arraylike, &length))
                    return JS_FALSE;
            }
            if (!arraylike) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_BAD_APPLY_ARGS, "apply");
                return JS_FALSE;
            }
        }
    }

    /* Convert the first arg to 'this' and skip over it. */
    if (!JSVAL_IS_PRIMITIVE(vp[2]))
        obj = JSVAL_TO_OBJECT(vp[2]);
    else if (!js_ValueToObject(cx, vp[2], &obj))
        return JS_FALSE;

    /* Allocate stack space for fval, obj, and the args. */
    argc = (uintN)JS_MIN(length, ARRAY_INIT_LIMIT - 1);
    invokevp = js_AllocStack(cx, 2 + argc, &mark);
    if (!invokevp)
        return JS_FALSE;

    /* Push fval, obj, and aobj's elements as args. */
    sp = invokevp;
    *sp++ = fval;
    *sp++ = OBJECT_TO_JSVAL(obj);
    for (i = 0; i < argc; i++) {
        ok = JS_GetElement(cx, aobj, (jsint)i, sp);
        if (!ok)
            goto out;
        sp++;
    }

    ok = js_Invoke(cx, argc, invokevp, JSINVOKE_INTERNAL);
    *vp = *invokevp;
out:
    js_FreeStack(cx, mark);
    return ok;
}

#ifdef NARCISSUS
static JSBool
fun_applyConstructor(JSContext *cx, uintN argc, jsval *vp)
{
    JSObject *aobj;
    uintN length, i;
    void *mark;
    jsval *invokevp, *sp;
    JSBool ok;

    if (JSVAL_IS_PRIMITIVE(vp[2]) ||
        (aobj = JSVAL_TO_OBJECT(vp[2]),
         OBJ_GET_CLASS(cx, aobj) != &js_ArrayClass &&
         OBJ_GET_CLASS(cx, aobj) != &js_ArgumentsClass)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_BAD_APPLY_ARGS, "__applyConstruct__");
        return JS_FALSE;
    }

    if (!js_GetLengthProperty(cx, aobj, &length))
        return JS_FALSE;

    if (length >= ARRAY_INIT_LIMIT)
        length = ARRAY_INIT_LIMIT - 1;
    invokevp = js_AllocStack(cx, 2 + length, &mark);
    if (!invokevp)
        return JS_FALSE;

    sp = invokevp;
    *sp++ = vp[1];
    *sp++ = JSVAL_NULL; /* This is filled automagically. */
    for (i = 0; i < length; i++) {
        ok = JS_GetElement(cx, aobj, (jsint)i, sp);
        if (!ok)
            goto out;
        sp++;
    }

    ok = js_InvokeConstructor(cx, invokevp, length);
    *vp = *invokevp;
out:
    js_FreeStack(cx, mark);
    return ok;
}
#endif

static JSFunctionSpec function_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,   fun_toSource,   0,0,0),
#endif
    JS_FN(js_toString_str,   fun_toString,   0,0,0),
    JS_FN("apply",           fun_apply,      0,2,0),
    JS_FN(call_str,          fun_call,       0,1,0),
#ifdef NARCISSUS
    JS_FN("__applyConstructor__", fun_applyConstructor, 0,1,0),
#endif
    JS_FS_END
};

static JSBool
Function(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval)
{
    JSStackFrame *fp, *caller;
    JSFunction *fun;
    JSObject *parent;
    uintN i, n, lineno;
    JSAtom *atom;
    const char *filename;
    JSBool ok;
    JSString *str, *arg;
    JSParseContext pc;
    JSPrincipals *principals;
    jschar *collected_args, *cp;
    void *mark;
    size_t arg_length, args_length, old_args_length;
    JSTokenType tt;

    fp = cx->fp;
    if (!(fp->flags & JSFRAME_CONSTRUCTING)) {
        obj = js_NewObject(cx, &js_FunctionClass, NULL, NULL);
        if (!obj)
            return JS_FALSE;
        *rval = OBJECT_TO_JSVAL(obj);
    }

    /*
     * The constructor is called before the private slot is initialized so we
     * must use JS_GetPrivate, not GET_FUNCTION_PRIVATE here.
     */
    fun = (JSFunction *) JS_GetPrivate(cx, obj);
    if (fun)
        return JS_TRUE;

    /*
     * NB: (new Function) is not lexically closed by its caller, it's just an
     * anonymous function in the top-level scope that its constructor inhabits.
     * Thus 'var x = 42; f = new Function("return x"); print(f())' prints 42,
     * and so would a call to f from another top-level's script or function.
     *
     * In older versions, before call objects, a new Function was adopted by
     * its running context's globalObject, which might be different from the
     * top-level reachable from scopeChain (in HTML frames, e.g.).
     */
    parent = OBJ_GET_PARENT(cx, JSVAL_TO_OBJECT(argv[-2]));

    fun = js_NewFunction(cx, obj, NULL, 0, JSFUN_LAMBDA | JSFUN_INTERPRETED,
                         parent, cx->runtime->atomState.anonymousAtom);

    if (!fun)
        return JS_FALSE;

    /*
     * Function is static and not called directly by other functions in this
     * file, therefore it is callable only as a native function by js_Invoke.
     * Find the scripted caller, possibly skipping other native frames such as
     * are built for Function.prototype.call or .apply activations that invoke
     * Function indirectly from a script.
     */
    JS_ASSERT(!fp->script && fp->fun && fp->fun->u.n.native == Function);
    caller = JS_GetScriptedCaller(cx, fp);
    if (caller) {
        filename = caller->script->filename;
        lineno = js_PCToLineNumber(cx, caller->script, caller->pc);
        principals = JS_EvalFramePrincipals(cx, fp, caller);
    } else {
        filename = NULL;
        lineno = 0;
        principals = NULL;
    }

    /* Belt-and-braces: check that the caller has access to parent. */
    if (!js_CheckPrincipalsAccess(cx, parent, principals,
                                  CLASS_ATOM(cx, Function))) {
        return JS_FALSE;
    }

    n = argc ? argc - 1 : 0;
    if (n > 0) {
        enum { OK, BAD, BAD_FORMAL } state;

        /*
         * Collect the function-argument arguments into one string, separated
         * by commas, then make a tokenstream from that string, and scan it to
         * get the arguments.  We need to throw the full scanner at the
         * problem, because the argument string can legitimately contain
         * comments and linefeeds.  XXX It might be better to concatenate
         * everything up into a function definition and pass it to the
         * compiler, but doing it this way is less of a delta from the old
         * code.  See ECMA 15.3.2.1.
         */
        state = BAD_FORMAL;
        args_length = 0;
        for (i = 0; i < n; i++) {
            /* Collect the lengths for all the function-argument arguments. */
            arg = js_ValueToString(cx, argv[i]);
            if (!arg)
                return JS_FALSE;
            argv[i] = STRING_TO_JSVAL(arg);

            /*
             * Check for overflow.  The < test works because the maximum
             * JSString length fits in 2 fewer bits than size_t has.
             */
            old_args_length = args_length;
            args_length = old_args_length + JSSTRING_LENGTH(arg);
            if (args_length < old_args_length) {
                JS_ReportOutOfMemory(cx);
                return JS_FALSE;
            }
        }

        /* Add 1 for each joining comma and check for overflow (two ways). */
        old_args_length = args_length;
        args_length = old_args_length + n - 1;
        if (args_length < old_args_length ||
            args_length >= ~(size_t)0 / sizeof(jschar)) {
            JS_ReportOutOfMemory(cx);
            return JS_FALSE;
        }

        /*
         * Allocate a string to hold the concatenated arguments, including room
         * for a terminating 0.  Mark cx->tempPool for later release, to free
         * collected_args and its tokenstream in one swoop.
         */
        mark = JS_ARENA_MARK(&cx->tempPool);
        JS_ARENA_ALLOCATE_CAST(cp, jschar *, &cx->tempPool,
                               (args_length+1) * sizeof(jschar));
        if (!cp) {
            JS_ReportOutOfMemory(cx);
            return JS_FALSE;
        }
        collected_args = cp;

        /*
         * Concatenate the arguments into the new string, separated by commas.
         */
        for (i = 0; i < n; i++) {
            arg = JSVAL_TO_STRING(argv[i]);
            arg_length = JSSTRING_LENGTH(arg);
            (void) js_strncpy(cp, JSSTRING_CHARS(arg), arg_length);
            cp += arg_length;

            /* Add separating comma or terminating 0. */
            *cp++ = (i + 1 < n) ? ',' : 0;
        }

        /* Initialize a tokenstream that reads from the given string. */
        if (!js_InitTokenStream(cx, &pc.tokenStream, collected_args,
                                args_length, NULL, filename, lineno)) {
            JS_ARENA_RELEASE(&cx->tempPool, mark);
            return JS_FALSE;
        }

        /* The argument string may be empty or contain no tokens. */
        tt = js_GetToken(cx, &pc.tokenStream);
        if (tt != TOK_EOF) {
            for (;;) {
                /*
                 * Check that it's a name.  This also implicitly guards against
                 * TOK_ERROR, which was already reported.
                 */
                if (tt != TOK_NAME)
                    goto after_args;

                /*
                 * Get the atom corresponding to the name from the tokenstream;
                 * we're assured at this point that it's a valid identifier.
                 */
                atom = CURRENT_TOKEN(&pc.tokenStream).t_atom;

                /* Check for a duplicate parameter name. */
                if (js_LookupLocal(cx, fun, atom, NULL) != JSLOCAL_NONE) {
                    const char *name;

                    name = js_AtomToPrintableString(cx, atom);
                    ok = name &&
                         js_ReportCompileErrorNumber(cx, &pc.tokenStream, NULL,
                                                     JSREPORT_WARNING |
                                                     JSREPORT_STRICT,
                                                     JSMSG_DUPLICATE_FORMAL,
                                                     name);
                    if (!ok)
                        goto after_args;
                }
                if (!js_AddLocal(cx, fun, atom, JSLOCAL_ARG))
                    goto after_args;

                /*
                 * Get the next token.  Stop on end of stream.  Otherwise
                 * insist on a comma, get another name, and iterate.
                 */
                tt = js_GetToken(cx, &pc.tokenStream);
                if (tt == TOK_EOF)
                    break;
                if (tt != TOK_COMMA)
                    goto after_args;
                tt = js_GetToken(cx, &pc.tokenStream);
            }
        }

        state = OK;
      after_args:
        if (state == BAD_FORMAL && !(pc.tokenStream.flags & TSF_ERROR)) {
            /*
             * Report "malformed formal parameter" iff no illegal char or
             * similar scanner error was already reported.
             */
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_FORMAL);
        }
        js_CloseTokenStream(cx, &pc.tokenStream);
        JS_ARENA_RELEASE(&cx->tempPool, mark);
        if (state != OK)
            return JS_FALSE;
    }

    if (argc) {
        str = js_ValueToString(cx, argv[argc-1]);
        if (!str)
            return JS_FALSE;
        argv[argc-1] = STRING_TO_JSVAL(str);
    } else {
        str = cx->runtime->emptyString;
    }

    ok = js_InitParseContext(cx, &pc, JSSTRING_CHARS(str), JSSTRING_LENGTH(str),
                             NULL, filename, lineno);
    if (ok) {
        js_InitCompilePrincipals(cx, &pc, principals);
        ok = js_CompileFunctionBody(cx, &pc, fun);
        js_FinishParseContext(cx, &pc);
    }
    return ok;
}

JSObject *
js_InitFunctionClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto;
    JSAtom *atom;
    JSFunction *fun;

    proto = JS_InitClass(cx, obj, NULL, &js_FunctionClass, Function, 1,
                         function_props, function_methods, NULL, NULL);
    if (!proto)
        return NULL;
    atom = js_Atomize(cx, js_FunctionClass.name, strlen(js_FunctionClass.name),
                      0);
    if (!atom)
        goto bad;
    fun = js_NewFunction(cx, proto, NULL, 0, JSFUN_INTERPRETED, obj, NULL);
    if (!fun)
        goto bad;
    fun->u.i.script = js_NewScript(cx, 1, 0, 0, 0, 0, 0);
    if (!fun->u.i.script)
        goto bad;
    fun->u.i.script->code[0] = JSOP_STOP;
    return proto;

bad:
    cx->weakRoots.newborn[GCX_OBJECT] = NULL;
    return NULL;
}

JSObject *
js_InitCallClass(JSContext *cx, JSObject *obj)
{
    JSObject *proto;

    proto = JS_InitClass(cx, obj, NULL, &js_CallClass, NULL, 0,
                         NULL, NULL, NULL, NULL);
    if (!proto)
        return NULL;

    /*
     * Null Call.prototype's proto slot so that Object.prototype.* does not
     * pollute the scope of heavyweight functions.
     */
    OBJ_SET_PROTO(cx, proto, NULL);
    return proto;
}

JSFunction *
js_NewFunction(JSContext *cx, JSObject *funobj, JSNative native, uintN nargs,
               uintN flags, JSObject *parent, JSAtom *atom)
{
    JSFunction *fun;
    JSTempValueRooter tvr;

    /* If funobj is null, allocate an object for it. */
    if (funobj) {
        OBJ_SET_PARENT(cx, funobj, parent);
    } else {
        funobj = js_NewObject(cx, &js_FunctionClass, NULL, parent);
        if (!funobj)
            return NULL;
    }

    /* Protect fun from any potential GC callback. */
    JS_PUSH_SINGLE_TEMP_ROOT(cx, OBJECT_TO_JSVAL(funobj), &tvr);

    /*
     * Allocate fun after allocating funobj so allocations in js_NewObject
     * and hooks called from it do not wipe out fun from newborn[GCX_FUNCTION].
     */
    fun = (JSFunction *) js_NewGCThing(cx, GCX_FUNCTION, sizeof(JSFunction));
    if (!fun)
        goto out;

    /* Initialize all function members. */
    fun->object = NULL;
    fun->nargs = nargs;
    fun->flags = flags & (JSFUN_FLAGS_MASK | JSFUN_INTERPRETED);
    if (flags & JSFUN_INTERPRETED) {
        JS_ASSERT(!native);
        JS_ASSERT(nargs == 0);
        fun->u.i.nvars = 0;
        fun->u.i.spare = 0;
        fun->u.i.script = NULL;
    } else {
        fun->u.n.native = native;
        fun->u.n.extra = 0;
        fun->u.n.minargs = 0;
    }
    fun->atom = atom;
    fun->clasp = NULL;

    /* Link fun to funobj and vice versa. */
    if (!js_LinkFunctionObject(cx, fun, funobj)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        fun = NULL;
    }

out:
    JS_POP_TEMP_ROOT(cx, &tvr);
    return fun;
}

void
js_TraceFunction(JSTracer *trc, JSFunction *fun)
{
    if (fun->object)
        JS_CALL_OBJECT_TRACER(trc, fun->object, "object");
    if (fun->atom)
        JS_CALL_STRING_TRACER(trc, ATOM_TO_STRING(fun->atom), "atom");
    if (FUN_INTERPRETED(fun) && fun->u.i.script)
        js_TraceScript(trc, fun->u.i.script);
}

void
js_FinalizeFunction(JSContext *cx, JSFunction *fun)
{
    /*
     * Null-check of i.script is required since the parser sets interpreted
     * very early.
     */
    if (FUN_INTERPRETED(fun) && fun->u.i.script)
        js_DestroyScript(cx, fun->u.i.script);
}

JSObject *
js_CloneFunctionObject(JSContext *cx, JSObject *funobj, JSObject *parent)
{
    JSObject *newfunobj;
    JSFunction *fun;

    JS_ASSERT(OBJ_GET_CLASS(cx, funobj) == &js_FunctionClass);
    newfunobj = js_NewObject(cx, &js_FunctionClass, NULL, parent);
    if (!newfunobj)
        return NULL;
    fun = GET_FUNCTION_PRIVATE(cx, funobj);
    if (!js_LinkFunctionObject(cx, fun, newfunobj)) {
        cx->weakRoots.newborn[GCX_OBJECT] = NULL;
        return NULL;
    }
    return newfunobj;
}

JSBool
js_LinkFunctionObject(JSContext *cx, JSFunction *fun, JSObject *funobj)
{
    if (!fun->object)
        fun->object = funobj;
    return JS_SetPrivate(cx, funobj, fun);
}

JSFunction *
js_DefineFunction(JSContext *cx, JSObject *obj, JSAtom *atom, JSNative native,
                  uintN nargs, uintN attrs)
{
    JSFunction *fun;

    fun = js_NewFunction(cx, NULL, native, nargs, attrs, obj, atom);
    if (!fun)
        return NULL;
    if (!OBJ_DEFINE_PROPERTY(cx, obj, ATOM_TO_JSID(atom),
                             OBJECT_TO_JSVAL(fun->object),
                             NULL, NULL,
                             attrs & ~JSFUN_FLAGS_MASK, NULL)) {
        return NULL;
    }
    return fun;
}

#if (JSV2F_CONSTRUCT & JSV2F_SEARCH_STACK)
# error "JSINVOKE_CONSTRUCT and JSV2F_SEARCH_STACK are not disjoint!"
#endif

JSFunction *
js_ValueToFunction(JSContext *cx, jsval *vp, uintN flags)
{
    jsval v;
    JSObject *obj;

    v = *vp;
    obj = NULL;
    if (JSVAL_IS_OBJECT(v)) {
        obj = JSVAL_TO_OBJECT(v);
        if (obj && OBJ_GET_CLASS(cx, obj) != &js_FunctionClass) {
            if (!OBJ_DEFAULT_VALUE(cx, obj, JSTYPE_FUNCTION, &v))
                return NULL;
            obj = VALUE_IS_FUNCTION(cx, v) ? JSVAL_TO_OBJECT(v) : NULL;
        }
    }
    if (!obj) {
        js_ReportIsNotFunction(cx, vp, flags);
        return NULL;
    }
    return GET_FUNCTION_PRIVATE(cx, obj);
}

JSObject *
js_ValueToFunctionObject(JSContext *cx, jsval *vp, uintN flags)
{
    JSFunction *fun;
    JSObject *funobj;
    JSStackFrame *caller;
    JSPrincipals *principals;

    if (VALUE_IS_FUNCTION(cx, *vp))
        return JSVAL_TO_OBJECT(*vp);

    fun = js_ValueToFunction(cx, vp, flags);
    if (!fun)
        return NULL;
    funobj = fun->object;
    *vp = OBJECT_TO_JSVAL(funobj);

    caller = JS_GetScriptedCaller(cx, cx->fp);
    if (caller) {
        principals = caller->script->principals;
    } else {
        /* No scripted caller, don't allow access. */
        principals = NULL;
    }

    if (!js_CheckPrincipalsAccess(cx, funobj, principals,
                                  fun->atom
                                  ? fun->atom
                                  : cx->runtime->atomState.anonymousAtom)) {
        return NULL;
    }
    return funobj;
}

JSObject *
js_ValueToCallableObject(JSContext *cx, jsval *vp, uintN flags)
{
    JSObject *callable;

    callable = JSVAL_IS_PRIMITIVE(*vp) ? NULL : JSVAL_TO_OBJECT(*vp);
    if (callable &&
        ((callable->map->ops == &js_ObjectOps)
         ? OBJ_GET_CLASS(cx, callable)->call
         : callable->map->ops->call)) {
        *vp = OBJECT_TO_JSVAL(callable);
    } else {
        callable = js_ValueToFunctionObject(cx, vp, flags);
    }
    return callable;
}

void
js_ReportIsNotFunction(JSContext *cx, jsval *vp, uintN flags)
{
    JSStackFrame *fp;
    uintN error;
    const char *name, *source;

    for (fp = cx->fp; fp && !fp->spbase; fp = fp->down)
        continue;
    name = NULL;
    source = NULL;
    if (flags & JSV2F_ITERATOR) {
        error = JSMSG_BAD_ITERATOR;
        name = js_iterator_str;
        source = js_ValueToPrintableSource(cx, *vp);
        if (!source)
            return;
    } else if (flags & JSV2F_CONSTRUCT) {
        error = JSMSG_NOT_CONSTRUCTOR;
    } else {
        error = JSMSG_NOT_FUNCTION;
    }

    js_ReportValueError3(cx, error,
                         (fp && fp->spbase <= vp && vp < fp->sp)
                         ? vp - fp->sp
                         : (flags & JSV2F_SEARCH_STACK)
                         ? JSDVG_SEARCH_STACK
                         : JSDVG_IGNORE_STACK,
                         *vp, NULL,
                         name, source);
}

JSBool
js_AddLocal(JSContext *cx, JSFunction *fun, JSAtom *atom, JSLocalKind kind)
{
    uint16 *indexp;
    JSPropertyOp getter;
    uintN readonly;

    JS_ASSERT(FUN_INTERPRETED(fun));
    JS_ASSERT(OBJ_IS_NATIVE(fun->object));
    if (kind == JSLOCAL_ARG) {
#if JS_HAS_DESTRUCTURING
        /*
         * Destructuring parameter does not have name so we just update the
         * number of arguments for it without adding a property.
         */
        if (!atom) {
            ++fun->nargs;
            return JS_TRUE;
        }
#endif
        indexp = &fun->nargs;
        getter = JS_HIDDEN_ARG_GETTER;
        readonly = 0;
    } else {
        JS_ASSERT(kind == JSLOCAL_VAR || kind == JSLOCAL_CONST);
        indexp = &fun->u.i.nvars;
        getter = JS_HIDDEN_VAR_GETTER;
        readonly = (kind == JSLOCAL_CONST) ? JSPROP_READONLY : 0;
    }

    if (*indexp == JS_BITMASK(16)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             (kind == JSLOCAL_ARG)
                             ? JSMSG_TOO_MANY_FUN_ARGS
                             :JSMSG_TOO_MANY_FUN_VARS);
        return JS_FALSE;
    }

    /*
     * To support duplicate parameter names we force a duplicate node on the
     * SCOPE_LAST_PROP(scope) list with the same id, distinguished by the
     * SPROP_ALLOW_DUPLICATE flag, and not mapped by an entry in scope. The
     * flag is cleared when js_AddNativeProperty finds that the property is
     * unique.
     */
    if (!js_AddNativeProperty(cx, fun->object,
                              JSID_HIDE_NAME(ATOM_TO_JSID(atom)),
                              getter, NULL, SPROP_INVALID_SLOT,
                              JSPROP_PERMANENT | JSPROP_SHARED | readonly,
                              SPROP_HAS_SHORTID | SPROP_ALLOW_DUPLICATE,
                              *indexp)) {
        return JS_FALSE;
    }

    /* Update the argument of variable counter. */
    ++*indexp;
    return JS_TRUE;
}

JSLocalKind
js_LookupLocal(JSContext *cx, JSFunction *fun, JSAtom *atom, uintN *indexp)
{
    jsid id;
    JSScope *scope;
    JSScopeProperty *sprop;
    JSLocalKind kind;

    JS_ASSERT(FUN_INTERPRETED(fun));
    JS_ASSERT(OBJ_IS_NATIVE(fun->object));
    kind = JSLOCAL_NONE;
    if (fun->nargs + fun->u.i.nvars != 0) {
        id = JSID_HIDE_NAME(ATOM_TO_JSID(atom));

        /*
         * Inline js_LookupPropertyWithFlags to avoid prototype chain search
         * and the resolve hook invocation. The former is just an optimization
         * while the latter is necessary to avoid early creation of lazy slots
         * in fun->objects. See comments in fun_resolve for details.
         */
        JS_LOCK_OBJ(cx, fun->object);
        scope = OBJ_SCOPE(fun->object);
        JS_ASSERT(scope->object == fun->object);
        sprop = SCOPE_GET_PROPERTY(scope, id);
        if (sprop) {
            JS_ASSERT(sprop->setter == NULL);
            if (sprop->getter == JS_HIDDEN_ARG_GETTER) {
                kind = JSLOCAL_ARG;
            } else {
                JS_ASSERT(sprop->getter == JS_HIDDEN_VAR_GETTER);
                kind = (sprop->attrs & JSPROP_READONLY)
                       ? JSLOCAL_CONST
                       : JSLOCAL_VAR;
            }
            if (indexp)
                *indexp = (uint16) sprop->shortid;
        }
        JS_UNLOCK_OBJ(cx, fun->object);
    }
    return kind;
}

JSAtom **
js_GetLocalNames(JSContext *cx, JSFunction *fun, JSArenaPool *pool,
                 uint32 **bitmap)
{
    uintN n, index;
    size_t allocsize;
    JSAtom **names;
    JSScopeProperty *sprop;
    JSBool setbit;
    uint32 bit;
#ifdef DEBUG
    uintN nvars = 0, nargs = 0;
#endif

    JS_ASSERT(FUN_INTERPRETED(fun));
    JS_ASSERT(OBJ_IS_NATIVE(fun->object));
    n = fun->nargs + fun->u.i.nvars;
    JS_ASSERT(n != 0);
    allocsize = n * sizeof *names;
    if (bitmap)
        allocsize += JS_HOWMANY(n, JS_BITS_PER_UINT32) * sizeof(uint32);
    JS_ARENA_ALLOCATE_CAST(names, JSAtom **, pool, allocsize);
    if (!names) {
        JS_ReportOutOfMemory(cx);
        return NULL;
    }

#if JS_HAS_DESTRUCTURING
    /* Some parameter names can be NULL due to destructuring patterns. */
    memset(names, 0, fun->nargs * sizeof *names);
#endif
    if (bitmap) {
        *bitmap = (uint32 *) (names + n);
        memset(*bitmap, 0, JS_HOWMANY(n, JS_BITS_PER_UINT32) * sizeof(uint32));
    }
    for (sprop = SCOPE_LAST_PROP(OBJ_SCOPE(fun->object));
         sprop; sprop = sprop->parent) {
        if (!JSID_IS_HIDDEN(sprop->id))
            continue;
        index = (uint16) sprop->shortid;
        if (sprop->getter == JS_HIDDEN_ARG_GETTER) {
            JS_ASSERT(nargs++ < fun->nargs);
            JS_ASSERT(index < fun->nargs);
            setbit = JS_TRUE;
        } else {
            JS_ASSERT(sprop->getter == JS_HIDDEN_VAR_GETTER);
            JS_ASSERT(nvars++ < fun->u.i.nvars);
            JS_ASSERT(index < fun->u.i.nvars);
            index += fun->nargs;
            setbit = (sprop->attrs & JSPROP_READONLY);
        }
        names[index] = JSID_TO_ATOM(JSID_UNHIDE_NAME(sprop->id));
        if (bitmap && setbit) {
            bit = JS_BIT(index & (JS_BITS_PER_UINT32 - 1));
            JS_ASSERT(((*bitmap)[index / JS_BITS_PER_UINT32] & bit) == 0);
            (*bitmap)[index / JS_BITS_PER_UINT32] |= bit;
        }
    }
#if !JS_HAS_DESTRUCTURING
    JS_ASSERT(nargs == fun->nargs);
#endif
    JS_ASSERT(nvars == fun->u.i.nvars);
    return names;
}

