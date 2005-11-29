/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
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
 * The Original Code is thebes gfx
 *
 * The Initial Developer of the Original Code is
 * mozilla.org.
 * Portions created by the Initial Developer are Copyright (C) 2005
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Vladimir Vukicevic <vladimir@pobox.com>
 *   Stuart Parmenter <pavlov@pavlov.net>
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

#include "nsIServiceManager.h"
#include "nsIPref.h"
#include "nsCRT.h"

#include "nsThebesDeviceContext.h"
#include "nsThebesRenderingContext.h"
#include "nsThebesDrawingSurface.h"

#include "nsIView.h"

#ifdef MOZ_ENABLE_GTK2
// for getenv
#include <stdlib.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include "nsFontMetricsUtils.h"
#include "nsFont.h"

#include <pango/pango.h>
#include <pango/pangox.h>
#include <pango/pango-fontmap.h>


#ifdef MOZ_ENABLE_GLITZ
#include "glitz-glx.h"
#endif /* GLITZ */
#endif /* GTK2 */

#ifdef MOZ_ENABLE_GTK2
#include "nsSystemFontsGTK2.h"
static nsSystemFontsGTK2 *gSystemFonts = nsnull;
#elif XP_WIN
#include <cairo-win32.h>
#include "nsSystemFontsWin.h"
static nsSystemFontsWin *gSystemFonts = nsnull;
#include <Usp10.h>
#else
#error Need to declare gSystemFonts!
#endif

#ifdef MOZ_ENABLE_GTK2
extern "C" {
static int x11_error_handler (Display *dpy, XErrorEvent *err) {
    NS_ASSERTION(PR_FALSE, "X Error");
    return 0;
}
}
#endif

#ifdef PR_LOGGING
PRLogModuleInfo* gThebesGFXLog = nsnull;
#endif

static NS_DEFINE_CID(kPrefCID, NS_PREF_CID);

NS_IMPL_ISUPPORTS_INHERITED0(nsThebesDeviceContext, DeviceContextImpl)

PRInt32 nsThebesDeviceContext::mDpi;

nsThebesDeviceContext::nsThebesDeviceContext()
{
#ifdef PR_LOGGING
    if (!gThebesGFXLog)
        gThebesGFXLog = PR_NewLogModule("thebesGfx");
#endif

    PR_LOG(gThebesGFXLog, PR_LOG_DEBUG, ("#### Creating DeviceContext %p\n", this));

    mDevUnitsToAppUnits = 1.0f;
    mAppUnitsToDevUnits = 1.0f;
    mCPixelScale = 1.0f;
    mZoom = 1.0f;

    mWidgetSurfaceCache.Init();

#ifdef XP_WIN
    SCRIPT_DIGITSUBSTITUTE sds;
    ScriptRecordDigitSubstitution(LOCALE_USER_DEFAULT, &sds);
#endif
}

nsThebesDeviceContext::~nsThebesDeviceContext()
{
  nsresult rv;
  nsCOMPtr<nsIPref> prefs = do_GetService(kPrefCID, &rv);
  if (NS_SUCCEEDED(rv)) {
    prefs->UnregisterCallback("browser.display.screen_resolution",
                              prefChanged, (void *)this);
  }
}

nsresult
nsThebesDeviceContext::SetDPI(PRInt32 aPrefDPI)
{
    PRInt32 OSVal;
    PRBool do_round = PR_TRUE;

#if defined(MOZ_ENABLE_GTK2)
    float screenWidthIn = float(::gdk_screen_width_mm()) / 25.4f;
    OSVal = NSToCoordRound(float(::gdk_screen_width()) / screenWidthIn);

    if (aPrefDPI > 0) {
        // If there's a valid pref value for the logical resolution,
        // use it.
        mDpi = aPrefDPI;
    } else if ((aPrefDPI == 0) || (OSVal > 72)) {
        // Either if the pref is 0 (force use of OS value) or the OS
        // value is bigger than 72, use the OS value.
        mDpi = OSVal;
    } else {
        // if we couldn't get the pref or it's negative, and the OS
        // value is under 72ppi, then use 72.
        mDpi = 72;
    }

#elif defined(XP_WIN)
    // XXX we should really look at the widget for printing and such, but this widget is currently always null...
    HDC dc = GetDC((HWND)nsnull);
    OSVal = GetDeviceCaps(dc, LOGPIXELSY);
    if (GetDeviceCaps(dc, TECHNOLOGY) != DT_RASDISPLAY)
        do_round = PR_FALSE;
    ReleaseDC((HWND)nsnull, dc);

    mDpi = OSVal;

#else
    mDpi = 72;
#endif

    int pt2t = 72;

    // make p2t a nice round number - this prevents rounding problems
    mPixelsToTwips = float(NSIntPointsToTwips(pt2t)) / float(mDpi);
    if (do_round)
        mPixelsToTwips = float(NSToIntRound(mPixelsToTwips));
    mTwipsToPixels = 1.0f / mPixelsToTwips;

    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::Init(nsNativeWidget aWidget)
{
    static int initialized = 0;
    PRInt32 prefVal = -1;
    if (!initialized) {
        initialized = 1;

        // Set prefVal the value of the preference
        // "browser.display.screen_resolution"
        // or -1 if we can't get it.
        // If it's negative, we pretend it's not set.
        // If it's 0, it means force use of the operating system's logical
        // resolution.
        // If it's positive, we use it as the logical resolution
        nsresult res;

        nsCOMPtr<nsIPref> prefs(do_GetService(kPrefCID, &res));
        if (NS_SUCCEEDED(res) && prefs) {
            res = prefs->GetIntPref("browser.display.screen_resolution", &prefVal);
            if (NS_FAILED(res)) {
                prefVal = -1;
            }
            prefs->RegisterCallback("browser.display.screen_resolution", prefChanged,
                                    (void *)this);
        }

        SetDPI(prefVal);
    } else {
        SetDPI(mDpi);
    }

    mWidget = aWidget;

    if (!mScreenManager)
        mScreenManager = do_GetService("@mozilla.org/gfx/screenmanager;1");
    if (!mScreenManager)
        return NS_ERROR_FAILURE;

    nsCOMPtr<nsIScreen> screen;
    mScreenManager->GetPrimaryScreen (getter_AddRefs(screen));
    if (screen) {
        PRInt32 x, y, width, height;
        screen->GetRect (&x, &y, &width, &height);
        mWidthFloat = float(width);
        mHeightFloat = float(height);
    }

#ifdef MOZ_ENABLE_GTK2
    if (getenv ("MOZ_X_SYNC")) {
        PR_LOG (gThebesGFXLog, PR_LOG_DEBUG, ("+++ Enabling XSynchronize\n"));
        XSynchronize (gdk_x11_get_default_xdisplay(), True);
        XSetErrorHandler(x11_error_handler);
    }
#endif

    mWidth = -1;
    mHeight = -1;

    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::CreateRenderingContext(nsIView *aView,
                                              nsIRenderingContext *&aContext)
{
    NS_ENSURE_ARG_POINTER(aView);
    NS_PRECONDITION(aView->HasWidget(), "View has no widget!");

    nsCOMPtr<nsIWidget> widget;
    widget = aView->GetWidget();

    return CreateRenderingContext(widget, aContext);
}

NS_IMETHODIMP
nsThebesDeviceContext::CreateRenderingContext(nsIDrawingSurface *aSurface,
                                              nsIRenderingContext *&aContext)
{
    nsresult rv;

    aContext = nsnull;
    nsCOMPtr<nsIRenderingContext> pContext;
    rv = CreateRenderingContextInstance(*getter_AddRefs(pContext));
    if (NS_SUCCEEDED(rv)) {
        rv = pContext->Init(this, aSurface);
        if (NS_SUCCEEDED(rv)) {
            aContext = pContext;
            NS_ADDREF(aContext);
        }
    }

    return rv;
}

NS_IMETHODIMP
nsThebesDeviceContext::CreateRenderingContext(nsIWidget *aWidget,
                                              nsIRenderingContext *&aContext)
{
    nsresult rv;

    aContext = nsnull;
    nsCOMPtr<nsIRenderingContext> pContext;
    rv = CreateRenderingContextInstance(*getter_AddRefs(pContext));
    if (NS_SUCCEEDED(rv)) {
        nsRefPtr<gfxASurface> surface(aWidget->GetThebesSurface());
        if (surface)
            rv = pContext->Init(this, surface);
        else
            rv = pContext->Init(this, aWidget);
        if (NS_SUCCEEDED(rv)) {
            aContext = pContext;
            NS_ADDREF(aContext);
        }
    }

    return rv;
}

NS_IMETHODIMP
nsThebesDeviceContext::CreateRenderingContext(nsIRenderingContext *&aContext)
{
    NS_ERROR("CreateRenderingContext with other rendering context arg; fix this if this needs to be called");
    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::CreateRenderingContextInstance(nsIRenderingContext *&aContext)
{
    nsCOMPtr<nsIRenderingContext> renderingContext = new nsThebesRenderingContext();
    if (!renderingContext)
        return NS_ERROR_OUT_OF_MEMORY;

    aContext = renderingContext;
    NS_ADDREF(aContext);

    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::SupportsNativeWidgets(PRBool &aSupportsWidgets)
{
    aSupportsWidgets = PR_TRUE;
    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::GetScrollBarDimensions(float &aWidth, float &aHeight) const
{
    aWidth = 10.0f * mPixelsToTwips;
    aHeight = 10.0f * mPixelsToTwips;
    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::GetSystemFont(nsSystemFontID aID, nsFont *aFont) const
{
    nsresult status = NS_OK;

    if (!gSystemFonts) {
#ifdef MOZ_ENABLE_GTK2
        gSystemFonts = new nsSystemFontsGTK2(mPixelsToTwips);
#elif XP_WIN
        gSystemFonts = new nsSystemFontsWin(mPixelsToTwips);
#else
#error Need to know how to create gSystemFonts, fix me!
#endif
    }

#ifdef XP_WIN
    gSystemFonts->GetSystemFont(aID, aFont);
    return NS_OK;

#else
    switch (aID) {
    case eSystemFont_Menu:         // css2
    case eSystemFont_PullDownMenu: // css3
        *aFont = gSystemFonts->GetMenuFont();
        break;

    case eSystemFont_Field:        // css3
    case eSystemFont_List:         // css3
        *aFont = gSystemFonts->GetFieldFont();
        break;

    case eSystemFont_Button:       // css3
        *aFont = gSystemFonts->GetButtonFont();
        break;

    case eSystemFont_Caption:      // css2
    case eSystemFont_Icon:         // css2
    case eSystemFont_MessageBox:   // css2
    case eSystemFont_SmallCaption: // css2
    case eSystemFont_StatusBar:    // css2
    case eSystemFont_Window:       // css3
    case eSystemFont_Document:     // css3
    case eSystemFont_Workspace:    // css3
    case eSystemFont_Desktop:      // css3
    case eSystemFont_Info:         // css3
    case eSystemFont_Dialog:       // css3
    case eSystemFont_Tooltips:     // moz
    case eSystemFont_Widget:       // moz
        *aFont = gSystemFonts->GetDefaultFont();
        break;
    }
#endif
    return status;
}

NS_IMETHODIMP
nsThebesDeviceContext::CheckFontExistence(const nsString& aFaceName)
{
    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::GetDepth(PRUint32& aDepth)
{
    aDepth = 24;
    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::GetPaletteInfo(nsPaletteInfo& aPaletteInfo)
{
    aPaletteInfo.isPaletteDevice = PR_FALSE;
    aPaletteInfo.sizePalette = 0;
    aPaletteInfo.numReserved = 0;
    aPaletteInfo.palette = nsnull;
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::ConvertPixel(nscolor aColor, PRUint32 & aPixel)
{
    aPixel = aColor;
    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::GetDeviceSurfaceDimensions(PRInt32 &aWidth, PRInt32 &aHeight)
{
    if (mWidth == -1)
        mWidth = NSToIntRound(mWidthFloat * mDevUnitsToAppUnits);

    if (mHeight == -1)
        mHeight = NSToIntRound(mHeightFloat * mDevUnitsToAppUnits);

    aWidth = mWidth;
    aHeight = mHeight;

    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::GetRect(nsRect &aRect)
{
    if (mWidget) {
        PRInt32 x,y;
        PRUint32 width, height;

#if defined (MOZ_ENABLE_GTK2)
        Window root_ignore;
        PRUint32 bwidth_ignore, depth;

        XGetGeometry(GDK_WINDOW_XDISPLAY(GDK_DRAWABLE(mWidget)),
                     GDK_WINDOW_XWINDOW(GDK_DRAWABLE(mWidget)),
                     &root_ignore, &x, &y,
                     &width, &height,
                     &bwidth_ignore, &depth);
#elif defined (XP_WIN)
        // XXX
        x = y = 0;
        width = height = 200;
#else
#error write me
#endif

        nsCOMPtr<nsIScreen> screen;
        mScreenManager->ScreenForRect(x, y, width, height, getter_AddRefs(screen));
        screen->GetRect(&aRect.x, &aRect.y, &aRect.width, &aRect.height);

        aRect.x = NSToIntRound(mDevUnitsToAppUnits * aRect.x);
        aRect.y = NSToIntRound(mDevUnitsToAppUnits * aRect.y);
        aRect.width = NSToIntRound(mDevUnitsToAppUnits * aRect.width);
        aRect.height = NSToIntRound(mDevUnitsToAppUnits * aRect.height);
    } else {
        aRect.x = 0;
        aRect.y = 0;

        this->GetDeviceSurfaceDimensions(aRect.width, aRect.height);
    }

    return NS_OK;
}

NS_IMETHODIMP
nsThebesDeviceContext::GetClientRect(nsRect &aRect)
{
    nsresult rv = this->GetRect(aRect);
    return rv;
}

NS_IMETHODIMP
nsThebesDeviceContext::PrepareNativeWidget(nsIWidget* aWidget, void** aOut)
{
    *aOut = nsnull;
    return NS_OK;
}

/*
 * below methods are for printing and are not implemented
 */
NS_IMETHODIMP
nsThebesDeviceContext::GetDeviceContextFor(nsIDeviceContextSpec *aDevice,
                                          nsIDeviceContext *&aContext)
{
    /* we don't do printing */
    return NS_ERROR_NOT_IMPLEMENTED;
}


NS_IMETHODIMP
nsThebesDeviceContext::PrepareDocument(PRUnichar * aTitle, 
                                      PRUnichar*  aPrintToFileName)
{
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::BeginDocument(PRUnichar*  aTitle, 
                                            PRUnichar*  aPrintToFileName,
                                            PRInt32     aStartPage, 
                                            PRInt32     aEndPage)
{
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::EndDocument(void)
{
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::AbortDocument(void)
{
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::BeginPage(void)
{
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::EndPage(void)
{
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::SetAltDevice(nsIDeviceContext* aAltDC)
{
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::GetAltDevice(nsIDeviceContext** aAltDC)
{
    *aAltDC = nsnull;
    return NS_OK;
}


NS_IMETHODIMP
nsThebesDeviceContext::SetUseAltDC(PRUint8 aValue, PRBool aOn)
{
    return NS_OK;
}

/** End printing methods **/

int
nsThebesDeviceContext::prefChanged(const char *aPref, void *aClosure)
{
    nsThebesDeviceContext *context = (nsThebesDeviceContext*)aClosure;
    nsresult rv;
  
    if (nsCRT::strcmp(aPref, "browser.display.screen_resolution") == 0) {
        PRInt32 dpi;
        nsCOMPtr<nsIPref> prefs(do_GetService(kPrefCID, &rv));
        rv = prefs->GetIntPref(aPref, &dpi);
        if (NS_SUCCEEDED(rv))
            context->SetDPI(dpi);
    }

    return 0;
}
