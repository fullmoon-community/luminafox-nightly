//* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
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
 * The Original Code is the Places Bookmark Properties.
 *
 * The Initial Developer of the Original Code is Google Inc.
 * Portions created by the Initial Developer are Copyright (C) 2006
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Joe Hughes <jhughes@google.com>
 *   Dietrich Ayala <dietrich@mozilla.com>
 *   Asaf Romano <mano@mozilla.com>
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

const BOOKMARK_ITEM = 0;
const BOOKMARK_FOLDER = 1;
const LIVEMARK_CONTAINER = 2;

const ACTION_EDIT = 0;
const ACTION_ADD = 1;
const ACTION_ADD_WITH_ITEMS = 2;

/**
 * Supported options:
 * BOOKMARK_ITEM : ACTION_EDIT, ACTION_ADD
 * BOOKMARK_FOLDER : ACTION_EDIT, ADD_WITH_ITEMS
 * LIVEMARK_CONTAINER : ACTION_EDIT
 */

var BookmarkPropertiesPanel = {

  /** UI Text Strings */
  __strings: null,
  get _strings() {
    if (!this.__strings) {
      this.__strings = document.getElementById("stringBundle");
    }
    return this.__strings;
  },

  /**
   * The Microsummary Service for displaying microsummaries.
   */
  __mss: null,
  get _mss() {
    if (!this.__mss)
      this.__mss = Cc["@mozilla.org/microsummary/service;1"].
                  getService(Ci.nsIMicrosummaryService);
    return this.__mss;
  },

  _action: null,
  _itemType: null,
  _bookmarkId: null,
  _bookmarkURI: null,
  _bookmarkTitle: undefined,
  _microsummaries: null,

  /**
   * Returns true if the microsummary field is visible in this variant
   * of the dialog.
   */
  _isMicrosummaryVisible: function BPP__isMicrosummaryVisible() {
    if (!("_microsummaryVisible" in this))
      this._microsummaryVisible = this._itemType == BOOKMARK_ITEM;

    return this._microsummaryVisible;
  },

  /**
   * This method returns the correct label for the dialog's "accept"
   * button based on the variant of the dialog.
   */
  _getAcceptLabel: function BPP__getAcceptLabel() {
    if (this._action == ACTION_ADD)
      return this._strings.getString("dialogAcceptLabelAdd");
    else if (this._action == ACTION_ADD_WITH_ITEMS)
      return this._strings.getString("dialogAcceptLabelAddMulti");

    return this._strings.getString("dialogAcceptLabelEdit");
  },

  /**
   * This method returns the correct title for the current variant
   * of this dialog.
   */
  _getDialogTitle: function BPP__getDialogTitle() {
    if (this._action == ACTION_ADD) {
      if (this._itemType == BOOKMARK_ITEM)
        return this._strings.getString("dialogTitleAdd");
      // Not yet supported, but the string exists
      else if (this._itemType == LIVEMARK_CONTAINER)
        return this._strings.getString("dialogTitleAddLivemark");
    }
    else if (this._action == ACTION_EDIT && this._itemType == BOOKMARK_FOLDER)
      return this._strings.getString("dialogTitleFolderEdit");
    else if (this._action == ACTION_ADD_WITH_ITEMS)
      return this._strings.getString("dialogTitleAddMulti");

    return this._strings.getString("dialogTitleBookmarkEdit");
  },

  /**
   * This method can be run on a URI parameter to ensure that it didn't
   * receive a string instead of an nsIURI object.
   */
  _assertURINotString: function BPP__assertURINotString(value) {
    NS_ASSERT((typeof(value) == "object") && !(value instanceof String),
    "This method should be passed a URI as a nsIURI object, not as a string.");
  },

  /**
   * Determines the correct variant of the dialog to display depending
   * on which action is passed in and the properties of the identifier value
   * (a URI, a bookmark ID or a folder ID).
   *
   * NOTE: It's currently not possible to create the dialog with a folder
   *       id and "add" mode.
   *
   * @param aIdentifier
   *        the URI or folder ID to display the properties for
   * @param aAction
   *        "add" if this is being triggered from an "add bookmark"
   *        UI action; or "editfolder" or "edititem" if this is being
   *        triggered from a "properties" UI action; or "addmulti" if
   *        we're trying to create multiple bookmarks.
   *
   */
  _determineVariant: function BPP__determineVariant(aIdentifier, aAction) {
    if (aAction == "add") {
      this._assertURINotString(aIdentifier);
      this._action = ACTION_ADD;
      this._itemType = BOOKMARK_ITEM;
    }
    else if (aAction == "addmulti") {
      this._action = ACTION_ADD_WITH_ITEMS
      this._itemType = BOOKMARK_FOLDER;
    }
    else if (typeof(aIdentifier) == "number") {
      if (aAction == "edititem") {
        this._action = ACTION_EDIT;
        this._itemType = BOOKMARK_ITEM;
      }
      if (aAction == "editfolder") {
        this._action = ACTION_EDIT;
        if (PlacesUtils.livemarks.isLivemark(aIdentifier))
          this._itemType = LIVEMARK_CONTAINER;
        else
          this._itemType = BOOKMARK_FOLDER;
      }
    }
  },

  /**
   * This method returns the title string corresponding to a given URI.
   * If none is available from the bookmark service (probably because
   * the given URI doesn't appear in bookmarks or history), we synthesize
   * a title from the first 100 characters of the URI.
   *
   * @param aURI
   *        nsIURI object for which we want the title
   *
   * @returns a title string
   */

  _getURITitleFromHistory: function BPP__getURITitleFromHistory(aURI) {
    this._assertURINotString(aURI);

    // get the title from History
    return PlacesUtils.history.getPageTitle(aURI);
  },

  /**
   * This method should be called by the onload of the Bookmark Properties
   * dialog to initialize the state of the panel.
   */
  onDialogLoad: function BPP_onDialogLoad() {
    this._tm = window.arguments[0];
    var action = window.arguments[1];
    var identifier = window.arguments[2];
    this._bookmarkTitle = window.arguments[3];

    this._determineVariant(identifier, action);

    if (this._action == ACTION_ADD) {
      // todo: livemark container support
      if (this._itemType == BOOKMARK_ITEM) {
        this._assertURINotString(identifier);
        this._bookmarkURI = identifier;
      }
    }
    else if (this._action == ACTION_ADD_WITH_ITEMS)
      this._URIList = identifier;
    else { // ACTION_EDIT
      if (this._itemType == BOOKMARK_ITEM) {
        this._bookmarkId = identifier;
        this._bookmarkURI =
          PlacesUtils.bookmarks.getBookmarkURI(this._bookmarkId);
        this._folderId = PlacesUtils.bookmarks.getFolderIdForItem(identifier);
      }
      else // bookmarks folder or a livemark container
        this._folderId = identifier;
    }

    this._initFolderTree();
    this._populateProperties();
    this._updateSize();
  },


  /**
   * This method initializes the folder tree.
   */
  _initFolderTree: function BPP__initFolderTree() {
    this._folderTree = this._element("folderTree");
    this._folderTree.peerDropTypes = [];
    this._folderTree.childDropTypes = [];
  },

  _initMicrosummaryPicker: function BPP__initMicrosummaryPicker() {
    try {
      this._microsummaries = this._mss.getMicrosummaries(this._bookmarkURI,
                                                         this._bookmarkURI);
    }
    catch(ex) {
      // There was a problem retrieving microsummaries; disable the picker.
      // The microsummary service will throw an exception in at least
      // two cases:
      // 1. the bookmarked URI contains a scheme that the service won't
      //    download for security reasons (currently it only handles http,
      //    https, and file);
      // 2. the page to which the URI refers isn't HTML or XML (the only two
      //    content types the service knows how to summarize).
      this._microsummaryVisible = false;
      this._element("microsummaryRow").hidden = true;
      return;
    }
    this._microsummaries.addObserver(this._microsummaryObserver);
    this._rebuildMicrosummaryPicker();
  },

  _element: function BPP__element(aID) {
    return document.getElementById(aID);
  },

  /**
   * This method fills in the data values for the fields in the dialog.
   */
  _populateProperties: function BPP__populateProperties() {
    /* The explicit comparison against undefined here allows creators to pass
     * "" to init() if they wish to have no title. */
    if (this._bookmarkTitle === undefined) {
      if (this._action == ACTION_ADD)
        this._bookmarkTitle = this._getURITitleFromHistory(this._bookmarkURI);
      else if (this._action == ACTION_ADD_WITH_ITEMS)
        this._bookmarkTitle = this._strings.getString("bookmarkAllTabsDefault");
      else { // ACTION_EDIT
        if (this._itemType == BOOKMARK_ITEM) {
          this._bookmarkTitle = PlacesUtils.bookmarks
                                           .getItemTitle(this._bookmarkId);
        }
        else  { // bookmarks folder or a livemark container
          this._bookmarkTitle =
            PlacesUtils.bookmarks.getFolderTitle(this._folderId);
        }
      }
    }

    document.title = this._getDialogTitle();
    document.documentElement.getButton("accept").label = this._getAcceptLabel();
    this._element("editTitleBox").value = this._bookmarkTitle;

    if (this._itemType == BOOKMARK_ITEM) {
      this._element("editURLBar").value = this._bookmarkURI.spec;
      var shortcutbox = this._element("editShortcutBox");
      shortcutbox.value =
        PlacesUtils.bookmarks.getKeywordForBookmark(this._bookmarkId);
    }
    else {
      this._element("locationRow").hidden = true;
      this._element("shortcutRow").hidden = true;
    }

    if (this._itemType == LIVEMARK_CONTAINER) {
      var feedURI = PlacesUtils.livemarks.getFeedURI(this._folderId);
      if (feedURI)
        this._element("editLivemarkFeedLocationBox").value = feedURI.spec;
      var siteURI = PlacesUtils.livemarks.getSiteURI(this._folderId);
      if (siteURI)
        this._element("editLivemarkSiteLocationBox").value = siteURI.spec;
    }
    else {
      this._element("livemarkFeedLocationRow").hidden = true;
      this._element("livemarkSiteLocationRow").hidden = true;
    }

    if (this._isMicrosummaryVisible())
      this._initMicrosummaryPicker();
    else
      this._element("microsummaryRow").hidden = true;

    if (this._action != ACTION_EDIT)
      this._folderTree.selectFolders([PlacesUtils.bookmarks.bookmarksRoot]);
    else
      this._element("folderRow").hidden = true;
  },

  //XXXDietrich - bug 370215 - update to use bookmark id once 360133 is fixed.
  _rebuildMicrosummaryPicker: function BPP__rebuildMicrosummaryPicker() {
    var microsummaryMenuList = this._element("microsummaryMenuList");
    var microsummaryMenuPopup = this._element("microsummaryMenuPopup");

    // Remove old items from the menu, except the first item, which represents
    // "don't show a microsummary; show the page title instead".
    while (microsummaryMenuPopup.childNodes.length > 1)
      microsummaryMenuPopup.removeChild(microsummaryMenuPopup.lastChild);

    var enumerator = this._microsummaries.Enumerate();
    while (enumerator.hasMoreElements()) {
      var microsummary = enumerator.getNext().QueryInterface(Ci.nsIMicrosummary);

      var menuItem = document.createElement("menuitem");

      // Store a reference to the microsummary in the menu item, so we know
      // which microsummary this menu item represents when it's time to save
      // changes to the datastore.
      menuItem.microsummary = microsummary;

      // Content may have to be generated asynchronously; we don't necessarily
      // have it now.  If we do, great; otherwise, fall back to the generator
      // name, then the URI, and we trigger a microsummary content update.
      // Once the update completes, the microsummary will notify our observer
      // to rebuild the menu.
      // XXX Instead of just showing the generator name or (heaven forbid)
      // its URI when we don't have content, we should tell the user that we're
      // loading the microsummary, perhaps with some throbbing to let her know
      // it's in progress.
      if (microsummary.content)
        menuItem.setAttribute("label", microsummary.content);
      else {
        menuItem.setAttribute("label", microsummary.generator ?
                                       microsummary.generator.name :
                                       microsummary.generatorURI.spec);
        microsummary.update();
      }

      microsummaryMenuPopup.appendChild(menuItem);

      // Select the item if this is the current microsummary for the bookmark.
      if (this._mss.isMicrosummary(this._bookmarkURI, microsummary))
        microsummaryMenuList.selectedItem = menuItem;
    }
  },

  _microsummaryObserver: {
    _owner: this,

    QueryInterface: function (aIID) {
      if (!aIID.equals(Ci.nsIMicrosummaryObserver) &&
          !aIID.equals(Ci.nsISupports))
        throw Cr.NS_ERROR_NO_INTERFACE;
      return this;
    },

    onContentLoaded: function(aMicrosummary) {
      this._owner._rebuildMicrosummaryPicker();
    },

    onElementAppended: function(aMicrosummary) {
      this._owner._rebuildMicrosummaryPicker();
    }
  },

  /**
   * Size the dialog to fit its contents.
   */
  _updateSize: function BPP__updateSize() {
    var width = window.outerWidth;
    window.sizeToContent();
    window.resizeTo(width, window.outerHeight);
  },

  onDialogUnload: function BPP_onDialogUnload() {
    if (this._isMicrosummaryVisible() && this._microsummaries)
      this._microsummaries.removeObserver(this._microsummaryObserver);
  },

  onDialogAccept: function BPP_onDialogAccept() {
    this._saveChanges();
  },

  /**
   * This method checks the current state of the input fields in the
   * dialog, and if any of them are in an invalid state, it will disable
   * the submit button.  This method should be called after every
   * significant change to the input.
   */
  validateChanges: function BPP_validateChanges() {
    document.documentElement.getButton("accept").disabled = !this._inputIsValid();
  },

  /**
   * This method checks to see if the input fields are in a valid state.
   *
   * @returns  true if the input is valid, false otherwise
   */
  _inputIsValid: function BPP__inputIsValid() {
    // When in multiple select mode, it's possible to deselect all rows,
    // but you have to file your bookmark in at least one folder.
    if (this._action != ACTION_EDIT &&
        this._folderTree.getSelectionNodes().length == 0)
      return false;

    if (this._itemType == BOOKMARK_ITEM && !this._containsValidURI("editURLBar"))
      return false;

    // Feed Location has to be a valid URI;
    // Site Location has to be a valid URI or empty
    if (this._itemType == LIVEMARK_CONTAINER) {
      if (!this._containsValidURI("editLivemarkFeedLocationBox"))
        return false;
      if (!this._containsValidURI("editLivemarkSiteLocationBox") &&
          (this._element("editLivemarkSiteLocationBox").value.length > 0))
        return false;
    }

    return true;
  },

  /**
   * Determines whether the XUL textbox with the given ID contains a
   * string that can be converted into an nsIURI.
   *
   * @param aTextboxID
   *        the ID of the textbox element whose contents we'll test
   *
   * @returns true if the textbox contains a valid URI string, false otherwise
   */
  _containsValidURI: function BPP__containsValidURI(aTextboxID) {
    try {
      var uri = PlacesUtils._uri(this._element(aTextboxID).value);
    } catch (e) {
      return false;
    }
    return true;
  },

  /**
   * Save any changes that might have been made while the properties dialog
   * was open.
   */
  _saveChanges: function BPP__saveChanges() {
    var transactions = [];
    var urlbox = this._element("editURLBar");
    var titlebox = this._element("editTitleBox");
    var newURI = this._bookmarkURI;
    if (this._itemType == BOOKMARK_ITEM)
      newURI = PlacesUtils._uri(urlbox.value);

    // adding one or more bookmarks
    if (this._action == ACTION_ADD || this._action == ACTION_ADD_WITH_ITEMS) {
      var folder = PlacesUtils.bookmarks.bookmarksRoot;
      var selected =  this._folderTree.getSelectionNodes();

      // add single bookmark
      if (this._action == ACTION_ADD) {
        // get folder id
        if (selected.length > 0) {
          var node = selected[0];
          if (PlacesUtils.nodeIsFolder(node) &&
              !PlacesUtils.nodeIsReadOnly(node))
            folder = asFolder(node).folderId;
        }
        var txnCreateItem = new PlacesCreateItemTransaction(newURI, folder, -1);
        txnCreateItem.childTransactions.push(
          new PlacesEditItemTitleTransaction(-1, titlebox.value));
        transactions.push(txnCreateItem);
      }
      // bookmark multiple URIs
      else {
        var folder = asFolder(selected[0]);

        var newFolderTrans = new
          PlacesCreateFolderTransaction(titlebox.value, folder.folderId, -1);

        for (var i = 0; i < this._URIList.length; ++i) {
          var uri = this._URIList[i];
          var txn = new PlacesCreateItemTransaction(uri, -1, -1);
          txn.childTransactions.push(
            new PlacesEditItemTitleTransaction(uri, this._getURITitleFromHistory(uri)));
          newFolderTrans.childTransactions.push(txn);
        }

        transactions.push(newFolderTrans);
      }
    }


    if (this._action == ACTION_EDIT) {
      // editing a bookmark
      if (this._itemType == BOOKMARK_ITEM) {
        transactions.push(
          new PlacesEditItemTitleTransaction(this._bookmarkId, titlebox.value));
      }
      // editing a livemark container or a folder
      else {
        transactions.push(
          new PlacesEditFolderTitleTransaction(this._folderId, titlebox.value));

        // editing a livemark container
        if (this._itemType == LIVEMARK_CONTAINER) {
          var feedURIString = this._element("editLivemarkFeedLocationBox").value;
          var feedURI = PlacesUtils._uri(feedURIString);
          transactions.push(
            new PlacesEditLivemarkFeedURITransaction(this._folderId, feedURI));

          // Site Location is empty, we can set its URI to null
          var siteURIString = this._element("editLivemarkSiteLocationBox").value;
          if (siteURIString) {
            siteURI = PlacesUtils._uri(siteURIString);
            transactions.push(
            new PlacesEditLivemarkSiteURITransaction(this._folderId,
                                                     PlacesUtils._uri(siteURIString)));
          }
        }
      }
    }

    if (this._itemType == BOOKMARK_ITEM) {
      // keyword
      var shortcutboxValue = this._element("editShortcutBox").value;
      if (shortcutboxValue) {
        transactions.push(
          new PlacesEditBookmarkKeywordTransaction(this._bookmarkId,
                                                   shortcutboxValue));
      }

      if (this._action == ACTION_EDIT &&
          (newURI.spec != this._bookmarkURI.spec)) {
        // XXXDietrich - needs to be transactionalized
        PlacesUtils.changeBookmarkURI(this._bookmarkId, newURI);
      }
    }

    // microsummaries
    // XXXmano: iconCount mess is here until we make the microsummary UI 2.0-like
    var menuList = this._element("microsummaryMenuList");
    if (this._isMicrosummaryVisible() && menuList.itemCount > 0) {
      // Something should always be selected in the microsummary menu,
      // but if nothing is selected, then conservatively assume we should
      // just display the bookmark title.
      if (menuList.selectedIndex == -1)
        menuList.selectedIndex = 0;

      // This will set microsummary == undefined if the user selected
      // the "don't display a microsummary" item.
      var newMicrosummary = menuList.selectedItem.microsummary;

      // Only add a microsummary update to the transaction if the microsummary
      // has actually changed, i.e. the user selected no microsummary,
      // but the bookmark previously had one, or the user selected a microsummary
      // which is not the one the bookmark previously had.
      // XXXDietrich - bug 370215 - update to use bookmark id once 360133 is fixed.
      if ((newMicrosummary == null &&
           this._mss.hasMicrosummary(this._bookmarkURI)) ||
          (newMicrosummary != null &&
           !this._mss.isMicrosummary(this._bookmarkURI, newMicrosummary))) {
        transactions.push(
          new PlacesEditBookmarkMicrosummaryTransaction(this._bookmarkURI,
                                                        newMicrosummary));
      }
    }

    // If we have any changes to perform, do them via the
    // transaction manager in the PlacesController so they can be undone.
    if (transactions.length > 0) {
      var aggregate =
        new PlacesAggregateTransaction(this._getDialogTitle(), transactions);
      this._tm.doTransaction(aggregate);
    }
  }
};
