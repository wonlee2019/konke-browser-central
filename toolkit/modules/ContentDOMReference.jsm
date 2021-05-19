/* vim: set ts=2 sw=2 sts=2 et tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * This module holds weak references to DOM elements that exist within the
 * current content process, and converts them to a unique identifier that can be
 * passed between processes. The identifer, if received by the same content process
 * that issued it, can then be converted back into the DOM element (presuming the
 * element hasn't had all of its other references dropped).
 *
 * The hope is that this module can eliminate the need for passing CPOW references
 * between processes during runtime.
 */

var EXPORTED_SYMBOLS = ["ContentDOMReference"];

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

XPCOMUtils.defineLazyServiceGetter(
  this,
  "finalizationService",
  "@mozilla.org/toolkit/finalizationwitness;1",
  "nsIFinalizationWitnessService"
);

/**
 * @typedef {number} ElementID
 * @typedef {Object} ElementIdentifier
 */

const FINALIZATION_TOPIC = "content-dom-reference-finalized";

// A WeakMap which ties finalization witness objects to the lifetime of the DOM
// nodes they're meant to witness. When the DOM node in the map key is
// finalized, the WeakMap stops holding the finalization witness in its value
// alive, which alerts our observer that the element has been destroyed.
const finalizerRoots = new WeakMap();

/**
 * An identifier generated by ContentDOMReference is a unique pair of BrowsingContext
 * ID and a numeric ID. gRegistry maps BrowsingContext's to an object with the following
 * properties:
 *
 *   IDToElement:
 *     A Map of IDs to WeakReference's to the elements they refer to.
 *
 *   elementToID:
 *     A WeakMap from a DOM element to an ID that refers to it.
 */
var gRegistry = new WeakMap();

var ContentDOMReference = {
  _init() {
    const { Services } = ChromeUtils.import(
      "resource://gre/modules/Services.jsm"
    );
    Services.obs.addObserver(this, FINALIZATION_TOPIC);
  },

  observe(subject, topic, data) {
    if (topic !== FINALIZATION_TOPIC) {
      throw new Error("Unexpected observer topic");
    }

    let identifier = JSON.parse(data);
    this._revoke(identifier);
  },

  /**
   * Generate and return an identifier for a given DOM element.
   *
   * @param {Element} element The DOM element to generate the identifier for.
   * @return {ElementIdentifier} The identifier for the DOM element that can be passed between
   * processes as a message.
   */
  get(element) {
    if (!element) {
      throw new Error(
        "Can't create a ContentDOMReference identifier for " +
          "non-existant nodes."
      );
    }

    let browsingContext = BrowsingContext.getFromWindow(element.ownerGlobal);
    let mappings = gRegistry.get(browsingContext);
    if (!mappings) {
      mappings = {
        IDToElement: new Map(),
        elementToID: new WeakMap(),
      };
      gRegistry.set(browsingContext, mappings);
    }

    let id = mappings.elementToID.get(element);
    if (id) {
      // We already had this element registered, so return the pre-existing ID.
      return { browsingContextId: browsingContext.id, id };
    }

    // We must be registering a new element at this point.
    id = Math.random();
    mappings.elementToID.set(element, id);
    mappings.IDToElement.set(id, Cu.getWeakReference(element));

    let identifier = { browsingContextId: browsingContext.id, id };

    finalizerRoots.set(
      element,
      finalizationService.make(FINALIZATION_TOPIC, JSON.stringify(identifier))
    );

    return identifier;
  },

  /**
   * Resolves an identifier back into the DOM Element that it was generated from.
   *
   * @param {ElementIdentifier} The identifier generated via ContentDOMReference.get for a
   * DOM element.
   * @return {Element} The DOM element that the identifier was generated for, or
   * null if the element does not still exist.
   */
  resolve(identifier) {
    let browsingContext = BrowsingContext.get(identifier.browsingContextId);
    let { id } = identifier;
    return this._resolveIDToElement(browsingContext, id);
  },

  /**
   * Removes an identifier from the registry so that subsequent attempts
   * to resolve it will result in null. This is done automatically when the
   * target node is GCed.
   *
   * @param {ElementIdentifier} The identifier to revoke, issued by ContentDOMReference.get for
   * a DOM element.
   */
  _revoke(identifier) {
    let browsingContext = BrowsingContext.get(identifier.browsingContextId);
    let { id } = identifier;

    let mappings = gRegistry.get(browsingContext);
    if (!mappings) {
      return;
    }

    mappings.IDToElement.delete(id);
  },

  /**
   * Private helper function that resolves a BrowsingContext and ID (the
   * pair that makes up an identifier) to a DOM element.
   *
   * @param {BrowsingContext} browsingContext The BrowsingContext that was hosting
   * the DOM element at the time that the identifier was generated.
   * @param {ElementID} id The ID generated for the DOM element.
   *
   * @return {Element} The DOM element that the identifier was generated for, or
   * null if the element does not still exist.
   */
  _resolveIDToElement(browsingContext, id) {
    let mappings = gRegistry.get(browsingContext);
    if (!mappings) {
      return null;
    }

    let weakReference = mappings.IDToElement.get(id);
    if (!weakReference) {
      return null;
    }

    return weakReference.get();
  },
};

ContentDOMReference._init();