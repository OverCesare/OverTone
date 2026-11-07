'use strict';
// The reader has its own layout/style tree. Navigation does not invalidate a
// long answer; commands and state still belong to the same C core and UI owner.
const uiDocuments = [document], uiDocumentSetup = [];
let readerDocument = null;
const $ = id => document.getElementById(id) || readerDocument?.getElementById(id);
const uiElements = selector => uiDocuments.flatMap(doc => [...doc.querySelectorAll(selector)]);
function onUIDocument(setup) { uiDocumentSetup.push(setup); uiDocuments.forEach(setup); }
function onUIEvent(...args) { onUIDocument(doc => doc.addEventListener(...args)); }
// All interface documents share this policy, including the isolated conversation reader.
onUIEvent('contextmenu', event => event.preventDefault(), {capture:true});
function activeUIElement() {
    const active = document.activeElement;
    return active === window.transcriptFrame ? readerDocument.activeElement : active;
}
function hiddenUIElement(element) {
    for (let node = element; node; node = node.ownerDocument.defaultView.frameElement)
        if (node.closest('[hidden],[inert],.inactive-view')) return true;
    return false;
}
function uiRect(element) {
    const rect = element.getBoundingClientRect(), frame = element.ownerDocument.defaultView.frameElement;
    if (!frame) return rect;
    const origin = frame.getBoundingClientRect();
    return new DOMRect(rect.x+origin.x,rect.y+origin.y,rect.width,rect.height);
}
// Attention cues do not block layout settlement or trigger frame-by-frame relayout.
function uiAnimations() { return uiDocuments.flatMap(doc => doc.getAnimations()).filter(a => !a.effect?.target?.hasAttribute('data-passive-motion')); }
function scriptMotion(animation) { return !('animationName' in animation || 'transitionProperty' in animation); }

document.addEventListener('DOMContentLoaded', () => {
    const transcript = $('transcript'), host = transcript.parentElement,
          frame = document.createElement('iframe');
    frame.className = 'reader-frame';
    frame.title = tr('Conversazione');
    // This is a layout boundary, not an untrusted HTML renderer. Existing DOM
    // handlers must execute; the inherited CSP and DOM-only answers still apply.
    frame.setAttribute('sandbox','allow-same-origin allow-scripts');
    host.insertBefore(frame,transcript);
    const doc = frame.contentDocument;
    doc.open();
    // Only this bundled shell is parsed as HTML. Answers always use text/DOM nodes.
    doc.write('<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="app.css"></head><body class="chat-panel"></body></html>');
    doc.close();
    window.transcriptFrame = frame;
    readerDocument = doc;
    doc.body.append(document.querySelector('.symbol-definitions').cloneNode(true),transcript);
    doc.body.style.background = 'var(--workspace-surface,var(--panel))';
    uiDocuments.push(doc);
    uiDocumentSetup.forEach(setup => setup(doc));
    function size() {
    const width = host.clientWidth;
        if (width && transcript.style.width !== width+'px') transcript.style.width = width+'px';
    }
    const attributes = ['lang','dir','data-theme','data-accent','data-code-wrap','data-input',
                        'data-text-highlight','data-overlay-scrollbars','data-chrome','data-fullscreen'];
    function appearance() {
        const root = document.documentElement, target = doc.documentElement;
        const title = tr('Conversazione');
        if (frame.title !== title) frame.title = title;
        for (const name of attributes) {
            const value = root.getAttribute(name);
            if (target.getAttribute(name) === value) continue;
            if (value === null) target.removeAttribute(name);
            else target.setAttribute(name,value);
        }
        target.classList.toggle('appearance-changing',root.classList.contains('appearance-changing'));
        for (const name of root.style) {
            if (name.startsWith('--mac-') || name.startsWith('--chrome-')) continue;
            const value = root.style.getPropertyValue(name);
            if (target.style.getPropertyValue(name) !== value) target.style.setProperty(name,value);
        }
        for (const name of [...target.style])
            if (!root.style.getPropertyValue(name)) target.style.removeProperty(name);
    }
    appearance();
    size();
    window.syncReaderVisibility = () => {
        const active = !$('chat-workspace').inert && !document.hidden;
        if (doc.documentElement.dataset.active !== String(active))
            doc.documentElement.dataset.active = String(active);
        if (!active) {
            suspendMotionSamples();
            window.finishReadingFlow?.();
            for (const animation of doc.getAnimations())
                if (scriptMotion(animation) && Number.isFinite(animation.effect?.getComputedTiming().endTime))
                    animation.finish();
        }
    };
    window.syncReaderVisibility();
    new MutationObserver(appearance).observe(document.documentElement,
        {attributes:true,attributeFilter:[...attributes,'style','class']});
    new ResizeObserver(size).observe(host);
    const pane = host.closest('.reading-pane');
    doc.addEventListener('focusin',() => pane.classList.toggle('reader-focused',doc.activeElement === transcript));
    doc.addEventListener('focusout',() => pane.classList.remove('reader-focused'));
});
