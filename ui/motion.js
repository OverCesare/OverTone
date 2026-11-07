'use strict';
// The browser owns animation cadence; no timer attempts to outrun the display.
const motionPreference = matchMedia('(prefers-reduced-motion:reduce)');
function motionEnabled() { return !document.hidden && !motionPreference.matches; }
function motionDocumentVisible(doc) {
    return !doc.hidden && (doc === document || doc.documentElement.dataset.active !== 'false');
}
// Refresh and verification actions share one icon slot and one lifecycle.
// Finish on a whole turn, including fast work; never restart an active rotation.
const activityFeedbacks = new Set();
function activityFeedback(control, {glyph = 'rotate-ccw', busyGlyph = 'rotate-ccw', finish = 'scan', canRun = () => true} = {}) {
    let cycle = null, timer = 0, busy = false, failed = false;
    control.dataset.activity = 'idle';
    const svg = control.querySelector(':scope>.icon'), use = svg.querySelector('use');
    function reset() {
        clearTimeout(timer);
        cycle?.animation?.cancel(); cycle = null;
        busy = failed = false;
        control.dataset.activity = 'idle';
        control.setAttribute('aria-busy','false');
        control.classList.remove('scan-complete','scan-failed');
        use.setAttribute('href','#i-'+glyph);
        control.disabled = !canRun();
    }
    function start() {
        if (cycle && !cycle.finishing) return;
        reset(); busy = true;
        cycle = {animation:null,finishing:false};
        control.dataset.activity = 'running';
        control.disabled = true;
        control.setAttribute('aria-busy','true');
        animate();
    }
    function animate() {
        if (!cycle || cycle.animation || !motionEnabled() || hiddenUIElement(control) || !control.getClientRects().length) return;
        use.setAttribute('href','#i-'+busyGlyph);
        cycle.animation = svg.animate([{transform:'rotate(0deg)'},{transform:'rotate(360deg)'}],
                                     {duration:800,iterations:Infinity,easing:'linear'});
        measureMotion('discovery',cycle.animation);
    }
    function complete(value) {
        if (cycle !== value || !value.finishing) return;
        if (finish === 'immediate') {reset();return;}
        value.animation?.cancel(); cycle = null;
        control.dataset.activity = failed ? 'error' : 'complete';
        control.setAttribute('aria-busy','false');
        control.disabled = !canRun();
        use.setAttribute('href',failed ? '#i-triangle-alert' : '#i-check');
        control.classList.add('scan-complete');
        control.classList.toggle('scan-failed',failed);
        timer = setTimeout(reset,2400);
    }
    function update(pending = busy, error = failed) {
        busy = pending; failed = error;
        if (busy && !cycle) start();
        const value = cycle;
        if (!value) return;
        const visible = motionEnabled() && !hiddenUIElement(control) && !!control.getClientRects().length;
        if (!visible) { value.animation?.cancel(); value.animation = null; }
        if (busy) {
            value.finishing = false;
            if (value.animation) value.animation.effect.updateTiming({iterations:Infinity});
            else animate();
        } else if (!value.finishing || !visible) {
            value.finishing = true;
            if (finish === 'immediate') {complete(value);return;}
            if (!value.animation || !visible) { complete(value); return; }
            const turns = Math.max(1,Math.ceil((Number(value.animation.currentTime) || 0)/800));
            value.animation.effect.updateTiming({iterations:turns});
            value.animation.finished.then(() => complete(value),() => {});
        }
    }
    const feedback = {start,update,reset,get active() { return !!cycle; },
        dispose:() => {reset();activityFeedbacks.delete(feedback);}};
    activityFeedbacks.add(feedback);
    return feedback;
}
function updateActivityVisibility() {
    for (const feedback of activityFeedbacks) feedback.update();
}
document.addEventListener('visibilitychange',updateActivityVisibility);
motionPreference.addEventListener('change',updateActivityVisibility);
// Sample only when two layout gestures overlap. WebKit interpolates the curve
// on the compositor, including on displays faster than the sample spacing.
function layoutSamples(start, end, sample) {
    const count = Math.ceil((end-start)/16);
    return Array.from({length:count+1}, (_,i) => {
        const offset=i/count;
        return {offset,...sample(start+(end-start)*offset)};
    });
}
function layoutEasing(timing) {
    const curve=new Animation(new KeyframeEffect(null,[],{...timing,fill:'both'}));
    return elapsed => {
        curve.currentTime=Math.max(0,Math.min(timing.duration,elapsed));
        return curve.effect.getComputedTiming().progress;
    };
}
function surfaceSlices(end, center, before, after, inset, axis = 'X') {
    return [[end,`translate${axis}(${before-after}px)`],
            [center,`scale${axis}(${Math.max(1,before-inset*2)/Math.max(1,after-inset*2)})`]];
}
function surfaceRect(element) {
    const box = element.getBoundingClientRect(), skin = element.sizeSkin;
    if (!skin) return box;
    const left = skin.firstElementChild.getBoundingClientRect(), right = skin.lastElementChild.getBoundingClientRect();
    return skin.axis === 'X' ? {left:left.left,top:box.top,right:right.right,bottom:box.bottom,
            width:right.right-left.left,height:box.height} :
        {left:box.left,top:left.top,right:box.right,bottom:right.bottom,width:box.width,height:right.bottom-left.top};
}
function hasSurfacePaint(paint) {
    return !!paint && !((paint.backgroundColor === 'rgba(0, 0, 0, 0)' || paint.backgroundColor === 'transparent') &&
        paint.boxShadow === 'none' && ['Top','Right','Bottom','Left'].every(edge => !parseFloat(paint['border'+edge+'Width'])));
}
// Animate only the paint of a changing rectangular surface. The end caps keep
// their corner radii; the centre stretches without scaling text or controls.
function resizeSurface(element, before, after, paint, timing, axis = 'X', path = null) {
    const horizontal = axis === 'X', dimension = horizontal ? 'width' : 'height',
          start = horizontal ? 'Left' : 'Top', end = horizontal ? 'Right' : 'Bottom',
          sides = horizontal ? ['top','bottom'] : ['left','right'],
          corners = horizontal ? [['TopLeft','BottomLeft'],['TopRight','BottomRight']] :
                                 [['TopLeft','TopRight'],['BottomLeft','BottomRight']];
    if (!path && Math.abs(before[dimension]-after[dimension]) < .5) return {effects:[],clean:() => {}};
    if (!hasSurfacePaint(paint)) return {effects:[],clean:() => {}};
    const transparent = paint.backgroundColor === 'rgba(0, 0, 0, 0)' || paint.backgroundColor === 'transparent';
    const previous = element.sizeSkin, effects = [],
          transition = previous?.originalTransition ?? element.style.transition,
          radius = Math.max(...['TopLeft','TopRight','BottomLeft','BottomRight']
              .map(corner => parseFloat(paint['border'+corner+'Radius']) || 0)),
          cap = Math.min(radius+2,Math.min(before[dimension],after[dimension])/2), inset = Math.max(0,cap-1),
          linear = horizontal && transparent && paint.boxShadow === 'none' && !radius &&
              !['Top','Right','Left'].some(edge => parseFloat(paint['border'+edge+'Width'])) && parseFloat(paint.borderBottomWidth)>0,
          skin = previous?.linear === linear ? previous : element.ownerDocument.createElement('span');
    if (previous && previous !== skin) previous.remove();
    skin.className = 'size-skin';
    skin.setAttribute('aria-hidden','true');
    Object.assign(skin.style,{left:-parseFloat(paint.borderLeftWidth)+'px',right:-parseFloat(paint.borderRightWidth)+'px',
        top:-parseFloat(paint.borderTopWidth)+'px',bottom:-parseFloat(paint.borderBottomWidth)+'px'});
    if (!skin.children.length) skin.append(...Array.from({length:linear ? 1 : 3},() => element.ownerDocument.createElement('i')));
    if (linear) Object.assign(skin.firstElementChild.style,{left:'0',right:'0',bottom:'0',
        height:paint.borderBottomWidth,backgroundColor:`var(--surface-stroke,${paint.borderBottomColor})`,transformOrigin:'left center'});
    for (let part=0;!linear && part<3;part++) {
        const slice = skin.children[part];
        for (const property of Object.keys(paint)) slice.style[property] = paint[property];
        if (!transparent) slice.style.backgroundColor = `var(--surface-fill,${paint.backgroundColor})`;
        slice.style.boxShadow = `var(--surface-shadow,${paint.boxShadow})`;
        for (const edge of ['Top','Right','Bottom','Left'])
            slice.style['border'+edge+'Color'] = `var(--surface-stroke,${paint['border'+edge+'Color']})`;
        slice.style[sides[0]] = slice.style[sides[1]] = '0';
        if (part === 1) {
            slice.style[start.toLowerCase()] = slice.style[end.toLowerCase()] = inset+'px';
            slice.style['border'+start+'Width'] = slice.style['border'+end+'Width'] = '0';
            Object.assign(slice.style,{borderRadius:'0',transformOrigin:'left top',
                clipPath:horizontal ? 'inset(-16px 0)' : 'inset(0 -16px)'});
        } else {
            slice.style[(part ? end : start).toLowerCase()] = '0';
            slice.style[dimension] = cap+'px';
            slice.style['border'+(part ? start : end)+'Width'] = '0';
            for (const corner of corners[part ? 0 : 1]) slice.style['border'+corner+'Radius'] = '0';
            slice.style.clipPath = horizontal ? part ? 'inset(-16px -16px -16px 0)' : 'inset(-16px 0 -16px -16px)' :
                                               part ? 'inset(0 -16px -16px)' : 'inset(-16px -16px 0)';
        }
    }
    // Keep authored first-child selectors (search buttons, headings) intact.
    if (skin.parentElement!==element) element.append(skin);
    element.sizeSkin = skin;
    skin.axis = axis;
    skin.linear = linear;
    skin.paint = paint;
    skin.originalTransition = transition;
    element.style.transition = 'none';
    element.setAttribute('data-size-motion','');
    const slices = linear ? [[skin.firstElementChild,`scaleX(${before.width/Math.max(1,after.width)})`]] :
        surfaceSlices(skin.lastElementChild,skin.children[1],before[dimension],after[dimension],inset,axis);
    for (const [node,transform] of slices) {
        if (!path) {
            effects.push(node.animate([{transform},{transform:'none'}],{...timing,fill:'both'}));
            continue;
        }
        const stretch = linear || node === skin.children[1],
              span = Math.max(1,after[dimension]-(linear ? 0 : 2*inset)),
              keys = path.points.map(({offset,length}) => ({offset,transform:stretch ?
                  `scale${axis}(${Math.max(1,span+length)/span})` : `translate${axis}(${length}px)`}));
        const track = node.animate(keys,path.timing);
        // Save the independent distances, so another click can change just
        // the inspector's destination while the catalogue continues moving.
        track.layoutDistances = [path.inspector,path.catalogue].map(d =>
            stretch ? [d/span,0,0,0] : [0,0,d,0]);
        track.startTime = path.start;
        effects.push(track);
    }
    return {effects,clean:() => {
        if (element.sizeSkin !== skin) return;
        element.sizeSkin = null;
        element.removeAttribute('data-size-motion');
        skin.remove();
        return () => { element.style.transition = transition; };
    }};
}
function surfacePaint(element) {
    if (element.sizeSkin) return element.sizeSkin.paint;
    const style = getComputedStyle(element), keys = ['backgroundColor','boxShadow'];
    for (const edge of ['Top','Right','Bottom','Left'])
        for (const property of ['Width','Style','Color']) keys.push('border'+edge+property);
    for (const corner of ['TopLeft','TopRight','BottomLeft','BottomRight']) keys.push('border'+corner+'Radius');
    return Object.fromEntries(keys.map(key => [key,style[key]]));
}
function revealContainers(element) {
    if (!element.revealSlot) {
        const slot = element.ownerDocument.createElement('div'), clip = element.ownerDocument.createElement('div');
        slot.className = 'reveal-slot';
        clip.className = 'reveal-clip';
        element.before(slot);
        slot.append(clip);
        clip.append(element);
        element.revealSlot = slot;
    }
    return {slot:element.revealSlot,clip:element.revealSlot.firstElementChild};
}
function revealEffects(clip, content, shown, height, open, timing) {
    const from = Math.min(height,shown)-height, to = open ? 0 : -height;
    return [
        clip.animate([{transform:`translateY(${from}px)`},{transform:`translateY(${to}px)`}],timing),
        content.animate([{transform:`translateY(${-from}px)`},{transform:`translateY(${-to}px)`}],timing)
    ];
}
// A notice and the following sections share one movement, including reversals.
function revealStack(element, open, {scope=null,surface=null}={}) {
    const previous = element.stackMotion;
    if (element.surfaceVisible === open && (previous || element.hidden === !open)) return;
    const {slot,clip} = revealContainers(element), height = clip.offsetHeight,
          shown = element.hidden ? 0 : Math.max(0,height+new DOMMatrixReadOnly(getComputedStyle(clip).transform).m42),
          siblings = [...(scope || slot.parentElement).children].filter(node => node !== slot && !node.contains(slot) && !hiddenUIElement(node)),
          before = siblings.map(node => node.getBoundingClientRect().top),
          surfaceBefore=surface ? surfaceRect(surface) : null,paint=surface ? surfacePaint(surface) : null;
    previous?.finish();
    element.surfaceVisible = open;
    element.hidden = element.inert = !open;
    if (!motionEnabled() || hiddenUIElement(slot)) return;
    element.hidden = false;
    slot.classList.toggle('collapsing',!open);
    if (!open) clip.style.height = height+'px';
    const timing = {...motionTiming(open ? 'normal' : 'exit'),fill:'both'},
          effects = revealEffects(clip,element,shown,clip.offsetHeight,open,timing),
          skin=surface ? resizeSurface(surface,surfaceBefore,surface.getBoundingClientRect(),paint,timing,'Y') : null;
    if (skin) effects.push(...skin.effects);
    siblings.forEach((node,index) => {
        const shift = before[index]-node.getBoundingClientRect().top;
        if (Math.abs(shift)>.25) effects.push(node.animate([{transform:`translateY(${shift}px)`},{transform:'none'}],timing));
    });
    const motion = element.stackMotion = {finish:() => {
        if (element.stackMotion !== motion) return;
        element.stackMotion = null;
        for (const effect of effects) effect.cancel();
        element.hidden = element.inert = !open;
        slot.classList.remove('collapsing');
        clip.style.height = '';
        const restore=skin?.clean();
        if (restore) {void getComputedStyle(surface).opacity;restore();}
    }};
    Promise.all(effects.map(effect => effect.finished)).then(motion.finish,() => {});
    measureMotion('surface',effects[0]);
}
let suggestionMotion = null;
function finishSuggestionMotion() {
    const motion = suggestionMotion;
    if (!motion) return;
    suggestionMotion = null;
    motion.observer.disconnect();
    for (const effect of motion.effects) effect.cancel();
    window.transcriptView.reflow(() => {
        motion.element.hidden = motion.element.inert = !motion.open;
        $('prompt').hidden = motion.open;
        motion.element.surfaceAnimation = null;
        motion.element.removeAttribute('data-surface-motion');
        motion.slot.classList.remove('collapsing');
        motion.clip.style.height = '';
        motion.pane.style.height = motion.paneHeight;
        motion.viewport.style.clipPath = motion.viewportClip;
        $('transcript').style.paddingBottom = motion.padding;
    });
    const restore = motion.clean();
    if (restore) { void getComputedStyle(motion.surface).opacity; restore(); }
}
// Reserve final layout once. Clip edges and their live contents move in opposite
// directions; the transcript's scroll range stays equal to its destination range.
function revealSuggestion(element, open, resized = false) {
    if (!resized && suggestionMotion?.open === open) return;
    if (!suggestionMotion && element.hidden === !open) {
        element.inert = !open;
        $('prompt').hidden = open;
        return;
    }
    const started = performance.now(), composer = $('composer'), surface = composer.querySelector('.composer-writing'),
          pane = document.querySelector('.reading-pane'), content = pane.querySelector('.reading-content'), viewport = pane.parentElement,
          transcript = $('transcript'), matrixY = node => new DOMMatrixReadOnly(getComputedStyle(node).transform).m42,
          before = surfaceRect(surface), paint = surfacePaint(surface),
          panelOpacity = element.hidden ? 0 : Number(getComputedStyle(element).opacity),
          draftOpacity = $('prompt').hidden ? 0 : Number(getComputedStyle($('prompt')).opacity),
          oldHeight = pane.offsetHeight+matrixY(pane), oldScroll = transcript.scrollTop,
          oldOffset = matrixY(pane)+matrixY(content),
          children = [...composer.querySelectorAll('[data-compose-anchor]')],
          positions = children.map(node => node.getBoundingClientRect().top),
          shown = element.hidden ? 0 : element.revealSlot ?
              element.revealSlot.firstElementChild.offsetHeight+matrixY(element.revealSlot.firstElementChild) : element.offsetHeight,
          remaining = resized ? Math.max(16,suggestionMotion.deadline-performance.now()) : null;
    finishSuggestionMotion();
    element.surfaceVisible = open;
    if (!motionEnabled() || hiddenUIElement(composer)) {
        window.transcriptView.reflow(() => {element.hidden = element.inert = !open;$('prompt').hidden=open;});
        return;
    }
    const {slot,clip} = revealContainers(element), effects = [],
          timing = {...motionTiming(open ? 'panel' : 'panel-exit'),fill:'both'},
          paneHeight = pane.style.height, viewportClip = viewport.style.clipPath, padding = transcript.style.paddingBottom,
          basePadding = parseFloat(getComputedStyle(transcript).paddingBottom);
    if (remaining !== null) timing.duration = remaining;
    let target, maximum;
    window.transcriptView.reflow(() => {
        element.hidden = false;
        element.inert = !open;
        $('prompt').hidden = open;
        window.fitComposerText(open ? $('suggestion') : $('prompt'));
        clip.style.height = '';
        const height = clip.offsetHeight;
        slot.classList.toggle('collapsing',!open);
        if (!open) clip.style.height = height+'px';
        target = pane.offsetHeight;
        maximum = Math.max(oldHeight,target);
        // Keep the stationary top edge, including while the bottom clip moves
        // beyond its reserved final height. Never paint over the chat heading.
        viewport.style.clipPath = `inset(0 0 ${target-maximum}px 0)`;
        pane.style.height = maximum+'px';
        transcript.style.paddingBottom = basePadding+maximum-target+'px';
    });
    // Two text surfaces must not become legible on top of each other during close.
    // On return, retire the outgoing review before revealing the original.
    effects.push(element.animate(open ?
        [{opacity:panelOpacity},{opacity:1,offset:.6},{opacity:1}] :
        [{opacity:panelOpacity},{opacity:0,offset:.4},{opacity:0}],timing));
    if (!open) effects.push($('prompt').animate(
        [{opacity:draftOpacity},{opacity:0,offset:.4},{opacity:1}],timing));
    const after = surface.getBoundingClientRect(), afterPositions = children.map(node => node.getBoundingClientRect().top),
          delta = before.top-after.top,
          from = oldHeight-maximum, to = target-maximum,
          compensation = oldOffset+transcript.scrollTop-oldScroll,
          size = resizeSurface(surface,before,after,paint,timing,'Y');
    effects.push(...size.effects,composer.animate([{transform:`translateY(${delta}px)`},{transform:'none'}],timing),
        pane.animate([{transform:`translateY(${from}px)`},{transform:`translateY(${to}px)`}],timing),
        content.animate([{transform:`translateY(${-from+compensation}px)`},{transform:`translateY(${-to}px)`}],timing),
        pane.querySelector('.reading-fade-top').animate(
            [{transform:`translateY(${-from}px)`},{transform:`translateY(${-to}px)`}],timing),
        ...revealEffects(clip,element,shown,clip.offsetHeight,open,timing));
    children.forEach((node,index) => {
        const shift = positions[index]-afterPositions[index]-delta;
        if (Math.abs(shift) >= .25)
            effects.push(node.animate([{transform:`translateY(${shift}px)`},{transform:'none'}],timing));
    });
    const observer = new ResizeObserver(() => {
        if (suggestionMotion?.observer === observer && composer.offsetHeight !== suggestionMotion.height)
            revealSuggestion(element,open,true);
    });
    const motion = suggestionMotion = {element,open,slot,clip,pane,paneHeight,viewport,viewportClip,padding,composer,surface,effects,
        clean:size.clean,observer,height:composer.offsetHeight,deadline:performance.now()+timing.duration};
    observer.observe(composer);
    element.surfaceAnimation = effects[0];
    element.setAttribute('data-surface-motion','');
    measureMotion('suggestion',effects[0],performance.now()-started);
    Promise.all(effects.map(effect => effect.finished)).then(() => {
        if (suggestionMotion === motion) finishSuggestionMotion();
    },() => {});
}
window.addEventListener('resize',finishSuggestionMotion);
let readingReveal = null;
function revealReadingBody(element, open) {
    element.surfaceVisible = open;
    if (!motionEnabled() || hiddenUIElement(element.parentElement)) {
        readingReveal?.finish({element,open});
        element.hidden = element.inert = !open;
        return Promise.resolve();
    }
    if (!readingReveal) readingReveal = readingFlow($('transcript'));
    return readingReveal.set(element,open);
}
// One transaction owns vertical reader motion. New toggles and streamed size
// changes retarget the same effects; overlapping reveals never fight over a row.
function readingFlow(root) {
    const intents = new Map(), observed = new Set();
    let effects = [], moving = new Map(), layout = new Map(), sizes = new Map(),
        rootTop = 0, scrollTop = 0, deadline = 0, revision = 0, disposed = false, sample = null;
    const matrixY = node => new DOMMatrixReadOnly(node.ownerDocument.defaultView.getComputedStyle(node).transform).m42;
    const observer = new ResizeObserver(() => {
        if (!disposed && [...sizes].some(([node,size]) =>
            Math.abs(node.offsetHeight-size.height) > 1 || Math.abs(node.offsetWidth-size.width) > 1))
            update(null,true);
    });
    function nodes(extra = null) {
        const result = new Set(root.children);
        for (const element of [...intents.keys(),extra].filter(Boolean))
            for (const node of element.closest('.message')?.children || []) result.add(node);
        return result;
    }
    function finish(change = null) {
        if (disposed) return;
        disposed = true;
        revision++;
        observer.disconnect();
        for (const animation of effects) animation.cancel();
        for (const [element,intent] of intents) {
            element.hidden = element.inert = !intent.open;
            element.surfaceAnimation = null;
            element.removeAttribute('data-surface-motion');
            intent.slot.classList.remove('collapsing');
            intent.clip.style.height = '';
            if (change?.element === element && change.open !== intent.open) intent.reject();
            else intent.resolve();
        }
        intents.clear();
        if (sample) finishMotion(sample,!!change || !motionDocumentVisible(root.ownerDocument));
        readingReveal = null;
    }
    function update(change = null, resized = false) {
        if (disposed) return;
        if (!motionEnabled() || !motionDocumentVisible(root.ownerDocument)) { finish(); return; }
        const started = performance.now(), view = root.getBoundingClientRect(), before = new Map(),
              candidates = nodes(change?.element), offsets = new Map(), shown = new Map();
        if (resized)
            for (const node of moving.keys()) offsets.set(node,matrixY(node));
        for (const node of candidates) {
            if (!node.isConnected) continue;
            let rect = node.getBoundingClientRect();
            if (resized && layout.has(node)) {
                let y = 0;
                for (let parent = node; parent && parent !== root; parent = parent.parentElement)
                    y += offsets.get(parent) || 0;
                const old = layout.get(node);
                rect = {...old,top:old.top+y+view.top-rootTop-(root.scrollTop-scrollTop)};
                rect.bottom = rect.top+rect.height;
            }
            if (rect.height && rect.bottom > view.top-view.height && rect.top < view.bottom+view.height)
                before.set(node,{top:rect.top,bottom:rect.bottom,height:rect.height,width:rect.width});
        }
        for (const [element,intent] of intents)
            shown.set(element,Math.max(0,intent.height+matrixY(intent.clip)));
        for (const animation of effects) animation.cancel();
        effects = [];
        moving = new Map();
        if (change) {
            const {element,open} = change, previous = intents.get(element), {slot,clip} = revealContainers(element);
            previous?.reject();
            let resolve, reject;
            const finished = new Promise((done,fail) => {resolve=done;reject=fail;});
            finished.catch(() => {});
            if (!shown.has(element)) shown.set(element,element.hidden ? 0 : clip.offsetHeight);
            intents.set(element,{open,slot,clip,height:clip.offsetHeight,resolve,reject,finished});
        }
        for (const [element,intent] of [...intents]) {
            if (!element.isConnected) { intent.reject();intents.delete(element);continue; }
            element.hidden = false;
            element.inert = !intent.open;
            intent.slot.classList.toggle('collapsing',!intent.open);
            intent.clip.style.height = intent.open ? '' : intent.height+'px';
        }
        if (!intents.size) {finish();return;}
        const timing = {...motionTiming(change?.open === false ? 'panel-exit' : 'panel'),fill:'both'};
        if (resized) timing.duration = Math.max(16,deadline-performance.now());
        else deadline = performance.now()+timing.duration;
        layout = new Map();
        const deltas = new Map();
        for (const node of candidates) {
            if (!node.isConnected) continue;
            const rect = node.getBoundingClientRect();
            layout.set(node,{top:rect.top,height:rect.height,width:rect.width});
            if (before.has(node)) deltas.set(node,before.get(node).top-rect.top);
        }
        rootTop = root.getBoundingClientRect().top;
        scrollTop = root.scrollTop;
        sizes = new Map();
        for (const [element,intent] of intents) {
            intent.height = intent.clip.getBoundingClientRect().height;
            const owner = element.closest('.message'), bounds = layout.get(owner);
            if (owner && bounds) sizes.set(owner,bounds);
        }
        for (const node of observed) if (!sizes.has(node)) {observer.unobserve(node);observed.delete(node);}
        for (const node of sizes.keys()) if (!observed.has(node)) {observer.observe(node);observed.add(node);}
        for (const [element,intent] of intents) {
            const animations = revealEffects(intent.clip,element,shown.get(element) || 0,intent.height,intent.open,timing);
            effects.push(...animations);
            element.surfaceAnimation = animations[0];
            element.setAttribute('data-surface-motion','');
        }
        for (const [node,total] of deltas) {
            let parent = node.parentElement;
            while (parent && parent !== root && !deltas.has(parent)) parent = parent.parentElement;
            const delta = total-(deltas.get(parent) || 0);
            if (Math.abs(delta) < .25) continue;
            const animation = node.animate([{transform:`translateY(${delta}px)`},{transform:'none'}],timing);
            moving.set(node,animation);
            effects.push(animation);
        }
        const episode = ++revision;
        for (const animation of effects) animation.finished.catch(() => {});
        const setup = performance.now()-started;
        if (!sample || motionSamples.get('reasoning') !== sample) {
            sample = beginMotionSample('reasoning',setup,root.ownerDocument);
            const initial = sample;
            effects[0].ready.then(() => {
                if (initial) initial.readyMs = Math.min(30000,performance.now()-initial.started);
            },() => {});
        } else sample.setupMs = Math.max(sample.setupMs,setup);
        effects[0].finished.then(() => {if (episode === revision) finish();},() => {});
    }
    return {finish,set:(element,open) => {
        if (intents.get(element)?.open !== open) update({element,open});
        return intents.get(element)?.finished || Promise.resolve();
    }};
}
window.finishReadingFlow = () => readingReveal?.finish();
window.addEventListener('resize',() => readingReveal?.finish());
// CSS is the source of timing tokens. Read once, before effects or large replies
// exist: querying computed style while preparing each effect can flush rendering.
const motionTimings = new Map();
function motionTiming(speed = 'normal') {
    if (!motionTimings.has(speed)) {
        const style = getComputedStyle(document.documentElement);
        for (const key of ['fast','normal','exit','meter','panel','panel-exit','menu','transfer','attention',speed]) {
            const timing = {duration:Number(style.getPropertyValue('--motion-'+key+'-ms')),
                            easing:style.getPropertyValue('--motion-ease').trim()};
            if (timing.duration && timing.easing) motionTimings.set(key,timing);
        }
    }
    const timing = motionTimings.get(speed);
    if (!timing) throw new Error('Missing motion timing');
    return {...timing};
}
// One bounded observer for concurrent motion. These are callback/CPU observations,
// not physical presentation timestamps or Core Animation's hitch metric.
const motionSamples = new Map(), motionWork = {chrome:0,reading:0,highlight:0,navigation:0,update:0};
let motionFrame = 0;
function recordMotionWork(kind, started) {
    if (current?.preferences.developer) motionWork[kind] += performance.now() - started;
}
function finishMotion(sample, interrupted = false) {
    if (motionSamples.get(sample.name) !== sample) return;
    motionSamples.delete(sample.name);
    sample.cleanup?.();
    const sorted = sample.gaps.slice().sort((a,b) => a-b),
          percentile = fraction => sorted[Math.max(0,Math.ceil(sorted.length*fraction)-1)] || 0,
          cadence = percentile(.5),
          value = {name:sample.name,startedAt:sample.startedAt,frames:sorted.length,
            medianMs:cadence,p95Ms:percentile(.95),p99Ms:percentile(.99),maximumMs:percentile(1),
            maximumAtMs:sample.maximumAt,
            arrivalMaxMs:sample.arrivalMax,lagMaxMs:sample.lagMax,
            lateCallbacks:sample.gaps.filter(gap => cadence && gap > cadence*1.5).length,
            callbackDebtMs:sample.gaps.reduce((sum,gap) => sum + (gap > cadence*1.5 ? gap-cadence : 0),0),
            setupMs:sample.setupMs,firstFrameMs:sample.firstFrameMs,readyMs:sample.readyMs,
            elapsedMs:Math.min(30000,performance.now()-sample.started),interrupted,
            inactive:!motionEnabled() || !motionDocumentVisible(sample.document)};
    for (const kind of Object.keys(motionWork))
        value[kind+'Ms'] = motionWork[kind] - sample.work[kind];
    if (current?.preferences.developer) send({command:'motionMetrics',value});
    if (!motionSamples.size) { cancelAnimationFrame(motionFrame); motionFrame = 0; }
}
function sampleMotionFrame(time) {
    motionFrame = 0;
    const arrived = performance.now();
    for (const sample of motionSamples.values()) {
        const inactive = !motionEnabled() || !motionDocumentVisible(sample.document);
        if (!current?.preferences.developer || sample.gaps.length >= 240 || inactive ||
            arrived-sample.started >= 2200) {
            finishMotion(sample,inactive);
            continue;
        }
        if (sample.last && sample.gaps.length < 240) {
            const gap = time-sample.last;
            sample.gaps.push(gap);
            if (gap > sample.maximumGap) {
                sample.maximumGap = gap;
                sample.maximumAt = arrived-sample.started;
            }
            sample.arrivalMax = Math.max(sample.arrivalMax,arrived-sample.arrived);
        } else if (!sample.last) sample.firstFrameMs = Math.max(0,arrived-sample.started);
        sample.lagMax = Math.max(sample.lagMax,Math.max(0,arrived-time));
        sample.last = time;
        sample.arrived = arrived;
    }
    if (motionSamples.size) motionFrame = requestAnimationFrame(sampleMotionFrame);
}
function beginMotionSample(name, setupMs = 0, ownerDocument = document) {
    if (!current?.preferences.developer || !motionEnabled() || !motionDocumentVisible(ownerDocument)) return null;
    const previous = motionSamples.get(name);
    if (previous) finishMotion(previous,true);
    const sample = {name,document:ownerDocument,startedAt:Date.now()/1000,started:performance.now(),setupMs,
        gaps:[],last:0,arrived:0,maximumGap:0,maximumAt:0,firstFrameMs:0,arrivalMax:0,lagMax:0,readyMs:0,work:{...motionWork}};
    motionSamples.set(name,sample);
    if (!motionFrame) motionFrame = requestAnimationFrame(sampleMotionFrame);
    return sample;
}
function measureMotion(name, animation, setupMs = 0) {
    if (!animation) return;
    window.queueGuidePlacement?.();
    const sample = beginMotionSample(name,setupMs,animation.effect?.target?.ownerDocument);
    if (!sample) return;
    animation.ready.then(() => sample.readyMs = Math.min(30000,performance.now()-sample.started),() => {});
    if (Number.isFinite(animation.effect?.getComputedTiming().endTime))
        animation.finished.then(() => finishMotion(sample),() => finishMotion(sample,true));
    else {
        // A scan can become finite after discovery completes. Observe both ends
        // without an unresolved finished promise on an indefinitely running loop.
        const done = () => finishMotion(sample), cancelled = () => finishMotion(sample,true);
        animation.addEventListener('finish',done);
        animation.addEventListener('cancel',cancelled);
        sample.cleanup = () => {
            animation.removeEventListener('finish',done);
            animation.removeEventListener('cancel',cancelled);
        };
    }
    return sample;
}
function suspendMotionSamples() {
    for (const sample of motionSamples.values())
        if (!motionEnabled() || !motionDocumentVisible(sample.document)) finishMotion(sample,true);
}
window.addEventListener('blur',suspendMotionSamples);
document.addEventListener('visibilitychange',suspendMotionSamples);
// CSS owns the lightweight repeating effects. Sample at most one short window,
// without retaining a pending finished promise for an infinite animation.
onUIEvent('animationstart', event => {
    if (!current?.preferences.developer) return;
    const name = event.target.closest('#brand-mark') ? 'startup' : 'indicator';
    if (motionSamples.has(name)) return;
    const animation = event.target.getAnimations().find(a => 'animationName' in a &&
                                                           a.animationName === event.animationName);
    measureMotion(name,animation);
});
onUIEvent('transitionrun', event => {
    if (!current?.preferences.developer || event.propertyName !== 'opacity') return;
    const target = event.target,
          name = target.matches('[popover]:not(#tooltip)') ? 'popover'
               : target.matches('#context-scrim') ? 'drawer' : null;
    if (!name) return;
    const animation = target.getAnimations().find(a => 'transitionProperty' in a && a.transitionProperty === 'opacity');
    measureMotion(name,animation);
});
