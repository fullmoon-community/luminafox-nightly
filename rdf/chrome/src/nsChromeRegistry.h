/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 * 
 *
 * Original Author: David W. Hyatt (hyatt@netscape.com)
 *
 * Contributor(s): 
 */

class nsIRDFService;
class nsIRDFDataSource;
class nsIRDFResource;
class nsICSSLoader;
class nsISimpleEnumerator;
class nsSupportsHashtable;
class nsIRDFContainer;
class nsIRDFContainerUtils;
class nsIDOMWindow;
class nsIDocument;

#include "nsIRDFCompositeDataSource.h"
#include "nsICSSStyleSheet.h"

class nsChromeRegistry : public nsIChromeRegistry
{
public:
  NS_DECL_ISUPPORTS

  // nsIChromeRegistry methods:
  NS_DECL_NSICHROMEREGISTRY

  // nsChromeRegistry methods:
  nsChromeRegistry();
  virtual ~nsChromeRegistry();

public:
  static nsresult FollowArc(nsIRDFDataSource *aDataSource,
                            nsCString& aResult, nsIRDFResource* aChromeResource,
                            nsIRDFResource* aProperty);

protected:
  NS_IMETHOD GetOverlayDataSource(nsIURI *aChromeURL, nsIRDFDataSource **aResult);
   
  nsresult GetResource(const nsCAutoString& aChromeType, nsIRDFResource** aResult);
  
  NS_IMETHOD UpdateOverlay(nsIRDFDataSource *aDataSource, nsIRDFResource *aResource, PRBool aRemove);
  NS_IMETHOD UpdateOverlays(nsIRDFDataSource *aDataSource,
                            PRBool aRemove);
  
  void LoadStyleSheet(nsICSSStyleSheet** aSheet, const nsCString & aURL);
  void GetUserSheetURL(nsCString & aURL);

private:
  NS_IMETHOD ReallyUpdateOverlayFromDataSource(const PRUnichar *aDocURI, char *aOverlayURI,
                                               PRBool aRemove);
  NS_IMETHOD LoadDataSource(const nsCAutoString &aFileName, nsIRDFDataSource **aResult,
                            PRBool aUseProfileDirOnly = PR_FALSE);

  NS_IMETHOD GetProfileRoot(nsCAutoString& aFileURL);
  NS_IMETHOD GetInstallRoot(nsCAutoString& aFileURL);

  NS_IMETHOD RefreshWindow(nsIDOMWindow* aWindow);

	NS_IMETHOD ProcessStyleSheet(nsIURL* aURL, nsICSSLoader* aLoader, nsIDocument* aDocument);

  NS_IMETHOD GetArcs(nsIRDFDataSource* aDataSource,
                        const nsCAutoString& aType,
                        nsISimpleEnumerator** aResult);

  NS_IMETHOD AddToCompositeDataSource(PRBool aUseProfile);
  
  NS_IMETHOD GetBaseURL(const nsCAutoString& aPackage, const nsCAutoString& aProvider, 
                             nsCAutoString& aBaseURL);

  NS_IMETHOD FindProvider(const nsCAutoString& aPackage,
                          const nsCAutoString& aProvider,
                          nsIRDFResource *aArc,
                          nsIRDFNode **aSelectedProvider);

  NS_IMETHOD SelectPackageInProvider(nsIRDFResource *aPackageList,
                                   const nsCAutoString& aPackage,
                                   const nsCAutoString& aProvider,
                                   const nsCAutoString& aProviderName,
                                   nsIRDFResource *aArc,
                                   nsIRDFNode **aSelectedProvider);

  NS_IMETHOD SetProvider(const nsCAutoString& aProvider,
                         nsIRDFResource* aSelectionArc,
                         const PRUnichar* aProviderName,
                         PRBool aAllUsers, PRBool aIsAdding);

  NS_IMETHOD SetProviderForPackage(const nsCAutoString& aProvider,
                                   nsIRDFResource* aPackageResource, 
                                   nsIRDFResource* aProviderPackageResource, 
                                   nsIRDFResource* aSelectionArc, 
                                   PRBool aAllUsers, PRBool aIsAdding);

  NS_IMETHOD SelectProviderForPackage(const nsCAutoString& aProviderType,
                                        const PRUnichar *aProviderName, 
                                        const PRUnichar *aPackageName, 
                                        nsIRDFResource* aSelectionArc, 
                                        PRBool aUseProfile, PRBool aIsAdding);

  NS_IMETHOD InstallProvider(const nsCAutoString& aProviderType,
                             const nsCAutoString& aBaseURL,
                             PRBool aUseProfile);

  void ProcessNewChromeBuffer(char *aBuffer, PRInt32 aLength);

protected:
  PRBool mInstallInitialized;
  PRBool mProfileInitialized;
  nsCAutoString mProfileRoot;
  nsCAutoString mInstallRoot;

  nsCOMPtr<nsIRDFCompositeDataSource> mChromeDataSource;
  nsIRDFDataSource* mUIDataSource; // [WEAK] Composite holds on to us.

  nsSupportsHashtable* mDataSourceTable;
  nsIRDFService* mRDFService;
  nsIRDFContainerUtils* mRDFContainerUtils;

  // Resources
  nsCOMPtr<nsIRDFResource> mSelectedSkin;
  nsCOMPtr<nsIRDFResource> mSelectedLocale;
  nsCOMPtr<nsIRDFResource> mBaseURL;
  nsCOMPtr<nsIRDFResource> mPackages;
  nsCOMPtr<nsIRDFResource> mPackage;
  nsCOMPtr<nsIRDFResource> mName;

  // Style Sheets
  nsCOMPtr<nsICSSStyleSheet> mScrollbarSheet;
  nsCOMPtr<nsICSSStyleSheet> mUserSheet;
};
