// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_PAGE_SCROLLING_ROOT_SCROLLER_CONTROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_PAGE_SCROLLING_ROOT_SCROLLER_CONTROLLER_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/heap/handle.h"

namespace blink {

class Document;
class Element;
class HTMLFrameOwnerElement;
class PaintLayer;

// Manages the root scroller associated with a given document. The root
// scroller causes browser controls movement, overscroll effects and prevents
// chaining scrolls up further in the DOM. It can be set from script using
// document.setRootScroller. High-level details are available in README.md.
//
// There are two notions of a root scroller in this class: m_rootScroller and
// m_effectiveRootScroller. The former is the Element that was set as the root
// scroller using document.setRootScroller. If the page didn't set a root
// scroller this will be nullptr. The "effective" root scroller is the current
// Node we're using internally to apply viewport scrolling actions.  Both these
// elements come from this controller's associated Document. The final "global"
// root scroller, the one whose scrolling hides browser controls, may be in a
// different frame.
//
// If the currently set m_rootScroller is a valid element to become the root
// scroller, it will be promoted to the effective root scroller. If it is not
// valid, the effective root scroller will fall back to the document Node. The
// rules for what makes an element a valid root scroller are set in
// isValidRootScroller(). The validity of the current root scroller is
// re-checked after each layout.
class CORE_EXPORT RootScrollerController
    : public GarbageCollected<RootScrollerController> {
 public:
  // Creates a RootScrollerController for the given document. Note: instances
  // of this class need to be made aware of layout updates.
  static RootScrollerController* Create(Document&);

  void Trace(blink::Visitor*);

  // Sets the element that will be used as the root scroller. This can be
  // nullptr, in which case we'll use the default element (documentElement) as
  // the effective root scroller.
  void Set(Element*);

  // Returns the element currently set as the root scroller from script. This
  // differs from the effective root scroller since the set Element may not
  // currently be a valid root scroller. e.g. If the page sets an Element
  // with `display: none`, get() will return that element, even though the
  // effective root scroller will remain the document Node.
  Element* Get() const;

  // This returns the Element that's actually being used to control viewport
  // actions right now. This is different from get() if a root scroller hasn't
  // been set, or if the set root scroller isn't currently a valid scroller.
  // See README.md for the difference between the root scroller from Get(), the
  // effective rootScroller, and the global RootScroller in
  // TopDocumentRootScrollerController.
  Node& EffectiveRootScroller() const;

  // This class needs to be informed of changes in layout so that it can
  // determine if the current root scroller is still valid or if it must be
  // replaced by the default root scroller.
  void DidUpdateLayout();

  // We need to track style changes to reevaluate the implicit root scroller.
  void DidUpdateStyle();

  // This class needs to be informed when the FrameView of its Document changes
  // size. This may occur without a layout (e.g. URL bar hiding) so we can't
  // rely on DidUpdateLayout.
  void DidResizeFrameView();

  // Called when an iframe in this document has an updated FrameView (e.g.
  // FrameView removed, swapped, etc.) so that we can recompute the effective
  // root scroller and set the appropriate properties on the view.
  void DidUpdateIFrameFrameView(HTMLFrameOwnerElement&);

  // Returns the PaintLayer associated with the currently effective root
  // scroller.
  PaintLayer* RootScrollerPaintLayer() const;

  // Used to determine which Element should scroll the viewport.  This is
  // needed since Blink's scrolling machinery works on Elements whereas the
  // document *Node* also scrolls so we need to designate an element one
  // Element as the viewport scroller. Sadly, this is *not* the
  // document.scrollingElement in general.
  bool ScrollsViewport(const Element&) const;

  void ElementRemoved(const Element&);

  // In the "implicit root scroller" mode, we might promote an element to
  // become the effective root scroller even if the page doesn't set it as so
  // to improve the user experience.  In this mode, as elements layout they'll
  // call this method and, if they meet the root scroller restrictions, will be
  // added to the implicit candidate set. After layout is done we'll go
  // through that set and select the best candidate.
  void ConsiderForImplicit(Node&);

 private:
  RootScrollerController(Document&);

  // Ensures the effective root scroller is currently valid and replaces it
  // with the default if not.
  void RecomputeEffectiveRootScroller();

  // Determines whether the given element meets the criteria to become the
  // effective root scroller.
  bool IsValidRootScroller(const Element&) const;

  // Determines whether the given element meets the criteria to be implicitly
  // set as the root scroller (in addition to being a valid root scroller).
  bool IsValidImplicit(const Element&) const;

  // Set certain properties to the effective root scroller. Called when a Node
  // becomes or unbecomes the effective root scroller.
  void ApplyRootScrollerProperties(Node&);

  void UpdateIFrameGeometryAndLayoutSize(HTMLFrameOwnerElement&) const;

  // Called after layout, runs through implicit candidates, removing ones that
  // are no longer meet the root scroller restrictions. Of the remaining ones,
  // will choose the best and set it as the implicit_root_scroller_.
  void ProcessImplicitCandidates();

  // The owning Document whose root scroller this object manages.
  WeakMember<Document> document_;

  // The Element that was set from script as rootScroller for this Document.
  // Depending on its validity to be the root scroller (e.g. a display: none
  // element isn't a valid root scroller), this may not actually be the
  // Element being used as the root scroller.
  WeakMember<Element> root_scroller_;

  // The Node currently being used as the root scroller in this Document.
  // If the m_rootScroller is valid this will point to it. Otherwise, it'll
  // use the document Node. It'll never be nullptr since the Document owns the
  // RootScrollerController.
  Member<Node> effective_root_scroller_;

  // Candidate Elements that we should examine after layout to determine which
  // should be root scroller. This is used when "implicit root scroller" is
  // enabled, where a valid Element can become the root scroller without being
  // explicitly set using document.setRootScroller.
  HeapHashSet<WeakMember<Element>> implicit_candidates_;

  WeakMember<Element> implicit_root_scroller_;

  bool document_has_document_element_;

  // This flag is used to force applicationn of rootScroller properties even if
  // the effective rootScroller doesn't change.
  bool needs_apply_properties_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_PAGE_SCROLLING_ROOT_SCROLLER_CONTROLLER_H_
