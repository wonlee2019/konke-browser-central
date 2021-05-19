/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {
  TYPES: { CONSOLE_MESSAGE },
} = require("devtools/server/actors/resources/index");
const { WebConsoleUtils } = require("devtools/server/actors/webconsole/utils");
const Targets = require("devtools/server/actors/targets/index");

const consoleAPIListenerModule = isWorker
  ? "devtools/server/actors/webconsole/worker-listeners"
  : "devtools/server/actors/webconsole/listeners/console-api";
const { ConsoleAPIListener } = require(consoleAPIListenerModule);

const { isArray } = require("devtools/server/actors/object/utils");

const {
  makeDebuggeeValue,
  createValueGripForTarget,
} = require("devtools/server/actors/object/utils");

const {
  getActorIdForInternalSourceId,
} = require("devtools/server/actors/utils/dbg-source");

/**
 * Start watching for all console messages related to a given Target Actor.
 * This will notify about existing console messages, but also the one created in future.
 *
 * @param TargetActor targetActor
 *        The target actor from which we should observe console messages
 * @param Object options
 *        Dictionary object with following attributes:
 *        - onAvailable: mandatory function
 *          This will be called for each resource.
 */
class ConsoleMessageWatcher {
  async watch(targetActor, { onAvailable }) {
    // The following code expects the ThreadActor to be instantiated, via:
    // prepareConsoleMessageForRemote > SourcesManager.getActorIdForInternalSourceId
    // The Thread Actor is instantiated via Target.attach, but we should
    // probably review this and only instantiate the actor instead of attaching the target.
    if (!targetActor.threadActor) {
      targetActor.attach();
    }

    // Bug 1642297: Maybe we could merge ConsoleAPI Listener into this module?
    const onConsoleAPICall = message => {
      onAvailable([
        {
          resourceType: CONSOLE_MESSAGE,
          message: prepareConsoleMessageForRemote(targetActor, message),
        },
      ]);
    };

    const isTargetActorContentProcess =
      targetActor.targetType === Targets.TYPES.PROCESS;

    // Only consider messages from a given window for all FRAME targets (this includes
    // WebExt and ParentProcess which inherits from BrowsingContextTargetActor)
    // But ParentProcess should be ignored as we want all messages emitted directly from
    // that process (window and window-less).
    // To do that we pass a null window and ConsoleAPIListener will catch everything.
    const window =
      targetActor.targetType === Targets.TYPES.FRAME &&
      targetActor.typeName != "parentProcessTarget"
        ? targetActor.window
        : null;

    const listener = new ConsoleAPIListener(window, onConsoleAPICall, {
      excludeMessagesBoundToWindow: isTargetActorContentProcess,
      ...(targetActor.consoleAPIListenerOptions || {}),
    });
    this.listener = listener;
    listener.init();

    // It can happen that the targetActor does not have a window reference (e.g. in worker
    // thread, targetActor exposes a workerGlobal property)
    const winStartTime =
      targetActor.window?.performance?.timing?.navigationStart || 0;

    const cachedMessages = listener.getCachedMessages(!targetActor.isRootActor);
    const messages = [];
    // Filter out messages that came from a ServiceWorker but happened
    // before the page was requested.
    for (const message of cachedMessages) {
      if (
        message.innerID === "ServiceWorker" &&
        winStartTime > message.timeStamp
      ) {
        continue;
      }
      messages.push({
        resourceType: CONSOLE_MESSAGE,
        message: prepareConsoleMessageForRemote(targetActor, message),
      });
    }
    onAvailable(messages);
  }

  /**
   * Stop watching for console messages.
   */
  destroy() {
    if (this.listener) {
      this.listener.destroy();
    }
  }

  /**
   * Called by devtools/server/actors/utils/logEvent.js, whenever a new
   * log point is triggered and request to spawn a console message
   *
   * @param Object message
   *        A fake nsIConsoleMessage, which looks like the one being generated by
   *        the platform API.
   */
  onLogPoint(message) {
    if (!this.listener) {
      throw new Error("This target actor isn't listening to console messages");
    }
    this.listener.handler(message);
  }
}

module.exports = ConsoleMessageWatcher;

/**
 * Return the properties needed to display the appropriate table for a given
 * console.table call.
 * This function does a little more than creating an ObjectActor for the first
 * parameter of the message. When layout out the console table in the output, we want
 * to be able to look into sub-properties so the table can have a different layout (
 * for arrays of arrays, objects with objects properties, arrays of objects, …).
 * So here we need to retrieve the properties of the first parameter, and also all the
 * sub-properties we might need.
 *
 * @param {TargetActor} targetActor: The Target Actor from which this object originates.
 * @param {Object} result: The console.table message.
 * @returns {Object} An object containing the properties of the first argument of the
 *                   console.table call.
 */
function getConsoleTableMessageItems(targetActor, result) {
  if (
    !result ||
    !Array.isArray(result.arguments) ||
    result.arguments.length == 0
  ) {
    return null;
  }

  const [tableItemGrip] = result.arguments;
  const dataType = tableItemGrip.class;
  const needEntries = ["Map", "WeakMap", "Set", "WeakSet"].includes(dataType);
  const ignoreNonIndexedProperties = isArray(tableItemGrip);

  const tableItemActor = targetActor.getActorByID(tableItemGrip.actor);
  if (!tableItemActor) {
    return null;
  }

  // Retrieve the properties (or entries for Set/Map) of the console table first arg.
  const iterator = needEntries
    ? tableItemActor.enumEntries()
    : tableItemActor.enumProperties({
        ignoreNonIndexedProperties,
      });
  const { ownProperties } = iterator.all();

  // The iterator returns a descriptor for each property, wherein the value could be
  // in one of those sub-property.
  const descriptorKeys = ["safeGetterValues", "getterValue", "value"];

  Object.values(ownProperties).forEach(desc => {
    if (typeof desc !== "undefined") {
      descriptorKeys.forEach(key => {
        if (desc && desc.hasOwnProperty(key)) {
          const grip = desc[key];

          // We need to load sub-properties as well to render the table in a nice way.
          const actor = grip && targetActor.getActorByID(grip.actor);
          if (actor) {
            const res = actor
              .enumProperties({
                ignoreNonIndexedProperties: isArray(grip),
              })
              .all();
            if (res?.ownProperties) {
              desc[key].ownProperties = res.ownProperties;
            }
          }
        }
      });
    }
  });

  return ownProperties;
}

/**
 * Prepare a message from the console API to be sent to the remote Web Console
 * instance.
 *
 * @param TargetActor targetActor
 *        The related target actor
 * @param object message
 *        The original message received from console-api-log-event.
 * @return object
 *         The object that can be sent to the remote client.
 */
function prepareConsoleMessageForRemote(targetActor, message) {
  const result = WebConsoleUtils.cloneObject(message);

  result.workerType = WebConsoleUtils.getWorkerType(result) || "none";
  result.sourceId = getActorIdForInternalSourceId(targetActor, result.sourceId);

  delete result.wrappedJSObject;
  delete result.ID;
  delete result.innerID;
  delete result.consoleID;

  if (result.stacktrace) {
    result.stacktrace = result.stacktrace.map(frame => {
      return {
        ...frame,
        sourceId: getActorIdForInternalSourceId(targetActor, frame.sourceId),
      };
    });
  }

  result.arguments = (message.arguments || []).map(obj => {
    const dbgObj = makeDebuggeeValue(targetActor, obj);
    return createValueGripForTarget(targetActor, dbgObj);
  });

  result.styles = (message.styles || []).map(string => {
    return createValueGripForTarget(targetActor, string);
  });

  if (result.level === "table") {
    const tableItems = getConsoleTableMessageItems(targetActor, result);
    if (tableItems) {
      result.arguments[0].ownProperties = tableItems;
      result.arguments[0].preview = null;
    }

    // Only return the 2 first params.
    result.arguments = result.arguments.slice(0, 2);
  }

  result.category = message.category || "webdev";
  result.innerWindowID = message.innerID;

  return result;
}