'use strict';
// Bounded reading window. SQLite owns branches and text; this cache owns only viewport continuity.
window.transcriptView = (() => {
    const element = $('transcript'), positions = new Map();
    let turns = [], older = 0, newer = 0, pending = null, anchor = null, follow = false, lastTop = 0;
    let latency = 200, velocity = 0, lastScroll = performance.now();
    let streamingFollow = true, stablePoint = null;
    let scrollMotion = null, scrollTimer = 0, scrollSample = null, scrollReveal = null, revealLatest = false;
    let refreshedTurn = 0, warmTimer = 0;
    function stopScroll() {
        clearTimeout(scrollTimer);
        scrollTimer = 0;
        scrollReveal?.cancel();
        scrollReveal = null;
        const moving = scrollMotion;
        scrollMotion = null;
        if (moving) {
            lastTop = element.scrollTop;
            element.scrollTo({top:lastTop,behavior:'auto'});
            stablePoint = point();
        }
        if (scrollSample) finishMotion(scrollSample,true);
        scrollSample = null;
    }
    const edge = latest => latest ? Math.max(0,element.scrollHeight-element.clientHeight) : 0;
    function finishScroll(expired = false) {
        const movement = scrollMotion;
        if (!movement) return;
        const target = edge(movement.latest);
        if (Math.abs(element.scrollTop-target) > 1) {
            // Streaming or a resize may have moved the destination. Ask the
            // native scroller for the new destination, without a direct jump.
            if (target !== movement.target) {
                movement.target = target;
                element.scrollTo({top:target,behavior:'smooth'});
                clearTimeout(scrollTimer);
                scrollTimer = setTimeout(() => finishScroll(true),4000);
            } else if (expired) stopScroll();
            // A scrollend queued by a cancelled earlier movement must not stop
            // the new destination. Only an idle timeout can conclude interruption.
            return;
        }
        clearTimeout(scrollTimer);
        scrollTimer = 0;
        scrollMotion = null;
        if (scrollSample) finishMotion(scrollSample);
        scrollSample = null;
        streamingFollow = movement.latest;
        stablePoint = point();
        lastTop = element.scrollTop;
        if (!movement.latest) page('older');
        navigation();
    }
    function scrollToEdge(latest = true) {
        const started = performance.now();
        stopScroll();
        const target = edge(latest);
        if (!motionEnabled() || Math.abs(element.scrollTop-target) <= 1) {
            streamingFollow = latest;
            if (latest) restorePosition(null, true);
            else {element.scrollTop = 0; page('older');}
            navigation();
            return;
        }
        streamingFollow = false;
        scrollMotion = {latest,target};
        const distance = target-element.scrollTop;
        if (Math.abs(distance) > element.clientHeight*3) {
            // A native long-distance scroll crosses tens of thousands of pixels
            // in ~200ms, outrunning tiled paint. Arrive within one reading frame
            // and reveal the destination, instead of flashing unpainted history.
            element.scrollTo({top:target-Math.sign(distance)*Math.min(320,element.clientHeight*.6),behavior:'auto'});
            const reveal = scrollReveal = element.animate([{opacity:0},{opacity:1}],motionTiming());
            reveal.finished.then(() => { if (scrollReveal === reveal) scrollReveal = null; },() => {});
        }
        scrollSample = beginMotionSample('scroll',performance.now()-started,element.ownerDocument);
        // WebKit's scrolling thread owns the movement at the display's rate.
        // No per-frame JS position writes or timer tries to outrun its compositor.
        element.scrollTo({top:target,behavior:'smooth'});
        scrollTimer = setTimeout(() => finishScroll(true),4000);
    }
    element.addEventListener('scrollend',() => finishScroll());
    motionPreference.addEventListener('change',() => {
        if (!motionPreference.matches || !scrollMotion) return;
        const latest = scrollMotion.latest;
        stopScroll();
        scrollToEdge(latest);
    });
    document.addEventListener('visibilitychange',() => { if (document.hidden) stopScroll(); });
    let lastLoad = 0, lastUpdate = 0, metricsSignature = '', updateStarted = 0, jump = null,
        lastReport = 0, reportTimer = 0, reportedShift = 0, reportedLost = false;
    function navigation() {
        if ($('chat-workspace').inert) return;
        const started = performance.now();
        const width = element.offsetWidth;
        if (width) {
            const overlay = String(width === element.clientWidth);
            if (document.documentElement.dataset.overlayScrollbars !== overlay)
                document.documentElement.dataset.overlayScrollbars = overlay;
        }
        const gap = element.scrollHeight - element.scrollTop - element.clientHeight;
        const host = window.transcriptFrame?.closest('.reading-pane') || element.parentElement;
        host.classList.toggle('fade-top',element.scrollTop > 2);
        host.classList.toggle('fade-bottom',gap > 2);
        $('latest').hidden = !current?.selected || (!newer &&
            (!current.active || current.activeChat !== current.selected || streamingFollow) &&
            gap < 100);
        // Keep inspection current after both gestures and animated navigation.
        // Bound updates while moving; one trailing report records the settled point.
        clearTimeout(reportTimer);
        if (performance.now() - lastReport > 250) report();
        reportTimer = setTimeout(report, 120);
        recordMotionWork('reading',started);
    }
    new ResizeObserver(navigation).observe(element);
    function report(shift = reportedShift, lost = reportedLost) {
        clearTimeout(reportTimer);
        reportTimer = 0;
        if ($('chat-workspace').inert) return;
        lastReport = performance.now();
        reportedShift = shift;
        reportedLost = lost;
        const metrics = {turns : turns.length, pending : !!pending, loadMs : Math.round(lastLoad),
                         shiftPx : Math.round(shift * 10) / 10, anchorLost : lost,
                         updateMs : Math.round(lastUpdate * 100) / 100, following: streamingFollow,
                         tailGap: Math.max(0, Math.round(element.scrollHeight - element.scrollTop - element.clientHeight)),
                         contentHeight:element.scrollHeight, viewportHeight:element.clientHeight};
        const signature = JSON.stringify(metrics);
        if (signature !== metricsSignature) {
            metricsSignature = signature;
            send({command : 'readingMetrics', value : metrics});
        }
    }
    function point() {
        if ($('chat-workspace').inert) return stablePoint;
        const box = element.getBoundingClientRect();
        if (!box.width || !box.height) return stablePoint;
        const top = box.top;
        const row = [...element.children].find(x => x.dataset.turn && x.getBoundingClientRect().bottom > top);
        if (!row) return null;
        const blocks = [...(row.querySelector('.message-body')?.children || [])],
              index = blocks.findIndex(node => node.getBoundingClientRect().bottom > top),
              target = index >= 0 ? blocks[index] : row;
        return {id : Number(row.dataset.turn), role : row.dataset.role, block : index,
                offset : target.getBoundingClientRect().top - top};
    }
    function request(direction, id) {
        if (pending || !current || waiting || current.storageFailed || editing && direction !== 'refresh')
            return false;
        pending = {direction, id, chat : current.selected, errorRevision:current.errorRevision,
                   started : performance.now()};
        report();
        send({command : 'page', chat : current.selected, id});
        return true;
    }
    function page(direction) {
        if (direction === 'latest') {
            endEdit(true);
            if (!newer && !pending) {
                scrollToEdge();
                return;
            }
            streamingFollow = true;
            if (pending?.direction === 'latest')
                return;
            pending = null;
            request(direction, 0);
        } else if (direction === 'older' && older)
            request(direction, older);
        else if (direction === 'newer' && newer)
            request(direction, newer);
    }
    function receive(state, previousChat) {
        updateStarted = performance.now();
        anchor = point();
        follow = streamingFollow && element.scrollHeight - element.scrollTop - element.clientHeight < 100;
        const changedChat = previousChat !== state.selected;
        const jumping = jump?.chat === state.selected && state.turns.some(t => t.id === jump.id);
        if (changedChat && previousChat) {
            positions.delete(previousChat);
            if (!follow && anchor)
                positions.set(previousChat, anchor);
            while (positions.size > 20)
                positions.delete(positions.keys().next().value);
        }
        if (changedChat) {
            window.finishReadingFlow?.();
            stopScroll();
            refreshedTurn = 0;
            streamingFollow = true;
            pending = null;
            // Returning to an active reply is an explicit request to see its progress,
            // independent of the reading position left in that conversation.
            const saved = jumping || state.active && state.activeChat === state.selected
                ? null : positions.get(state.selected);
            if (saved && saved.id !== state.head && request('restore', saved.id)) {
                turns = [];
                return {turns, loading : true};
            }
            anchor = saved || null;
            follow = !saved;
        }
        const received = pending?.chat === state.selected &&
            (pending.id === state.page || (pending.id === state.head && !state.page));
        let direction = received ? pending.direction : '';
        if (jumping) {
            const target = state.turns.find(t => t.id === jump.id);
            const query = state.query.toLocaleLowerCase();
            anchor = {id : jump.id, role : target.prompt.toLocaleLowerCase().includes(query) ? 'user' : 'assistant', offset : 0};
            follow = false;
            direction = 'jump';
            pending = null;
            jump = null;
        }
        if (received) {
            lastLoad = performance.now() - pending.started;
            latency = Math.min(2000, .75 * latency + .25 * lastLoad);
        }
        if (received && direction === 'restore')
            anchor = positions.get(state.selected) || null;
        const requestFailed = state.storageFailed || state.error && !state.errorTurn &&
            state.errorRevision !== pending?.errorRevision &&
            (!state.errorChat || state.errorChat === state.selected);
        if (pending && !received && !requestFailed)
            return {turns, loading : !turns.length};
        if (received || requestFailed)
            pending = null;
        if (changedChat || ['latest', 'restore', 'jump'].includes(direction) || !turns.length) {
            turns = state.turns;
            older = state.older || 0;
            newer = state.newer || 0;
            if (direction === 'latest') {
                anchor = null;
                follow = true;
                revealLatest = !changedChat;
            }
        } else {
            const merged = new Map(turns.map(t => [t.id, t]));
            for (const turn of state.turns)
                merged.set(turn.id, turn);
            if (!state.page) {
                // A retry/edit can replace a branch. Keep only ancestors of the actual selected head.
                const chain = new Set();
                for (let id = state.head; id && merged.has(id); id = merged.get(id).parent)
                    chain.add(id);
                for (const id of merged.keys())
                    if (!chain.has(id))
                        merged.delete(id);
                newer = 0;
            }
            turns = [...merged.values()].sort((a, b) => a.id - b.id);
            if (!state.page)
                older = turns[0]?.parent || 0;
            if (direction === 'older')
                older = state.older || 0;
            if (direction === 'newer')
                newer = state.newer || 0;
        }
        let bytes = turns.reduce((n, t) => n + 2 * (t.prompt.length + t.answer.length + t.reasoning.length), 0);
        let trimmed = false;
        while (turns.length > 96 || (turns.length > 24 && bytes > 2 * 1024 * 1024)) {
            const removed = direction === 'older' ? turns.pop() : turns.shift();
            bytes -= 2 * (removed.prompt.length + removed.answer.length + removed.reasoning.length);
            if (direction === 'older' && !trimmed)
                newer = removed.id;
            else
                if (direction !== 'older') older = removed.id;
            trimmed = true;
        }
        $('older').hidden = !older;
        $('latest').hidden = !newer && (follow || !state.selected);
        // A warm/older page need not contain the live turn. Once generation ends,
        // refresh its durable record before leaving a cached turn marked pending.
        // The contiguous reading cache keeps its anchor; this is not a jump to latest.
        const stale = turns.find(t => t.state === 'pending');
        if (state.activeChat !== state.selected && !state.storageFailed && stale &&
            refreshedTurn !== stale.id)
            queueMicrotask(() => {
                if (current?.selected === state.selected && current.activeChat !== current.selected &&
                    turns.some(t => t.id === stale.id && t.state === 'pending') && request('refresh', 0))
                    refreshedTurn = stale.id;
            });
        return {turns, loading : false};
    }
    function restorePosition(saved, atBottom) {
        let shift = 0, lost = false;
        // A hidden workspace has no reading geometry. Keep its anchor until it
        // is shown, instead of replacing it with a zero-size layout observation.
        if ($('chat-workspace').inert || !element.getClientRects().length) return {shift, lost};
        if (saved) {
            const row = [...element.children].find(x => Number(x.dataset.turn) === saved.id &&
                                                        x.dataset.role === saved.role);
            if (row) {
                const target = saved.block >= 0 ? row.querySelector('.message-body')?.children[saved.block] || row : row;
                element.scrollTop += target.getBoundingClientRect().top - element.getBoundingClientRect().top - saved.offset;
                shift = target.getBoundingClientRect().top - element.getBoundingClientRect().top - saved.offset;
            } else
                lost = true;
        } else if (atBottom) {
            element.scrollTop = element.scrollHeight;
        }
        lastTop = element.scrollTop;
        return {shift, lost};
    }
    function reflow(change, preserve = false) {
        if (preserve)
            streamingFollow = false;
        const saved = point(), atBottom = !preserve && element.scrollHeight - element.scrollTop - element.clientHeight < 100,
              started = performance.now();
        change();
        const {shift, lost} = restorePosition(saved && !atBottom ? saved : null, atBottom);
        lastUpdate = performance.now() - started;
        stablePoint = point();
        navigation();
        report(shift, lost);
    }
    function rendered() {
        if (!follow && anchor)
            streamingFollow = false;
        const {shift, lost} = scrollMotion ? {shift:0, lost:false} : restorePosition(follow ? null : anchor, follow);
        if (revealLatest) {
            revealLatest = false;
            if (motionEnabled()) {
                // A distant page replaces a bounded window. Animate its arrival without
                // loading or retaining every intervening message just for a scroll.
                element.scrollTop = Math.max(0, element.scrollTop - Math.min(180, element.clientHeight / 3));
                element.animate([{opacity:0,transform:'translateY(12px)'},{opacity:1,transform:'translateY(0)'}],
                                 motionTiming());
                scrollToEdge();
            }
        }
        anchor = null;
        lastUpdate = updateStarted ? performance.now() - updateStarted : 0;
        updateStarted = 0;
        report(shift, lost);
        stablePoint = point();
        navigation();
        // Warm adjacent history after interactive motion settles, not inside its
        // next frame. Actual scrolling still requests its own adaptive read-ahead.
        clearTimeout(warmTimer);
        if (!current?.active && !newer && older && turns.length < 48)
            warmTimer = setTimeout(warmHistory, 150);
    }
    function warmHistory() {
        warmTimer = 0;
        if (pending || current?.active || newer || !older || turns.length >= 48 ||
            document.hidden || $('chat-workspace').inert || !element.getClientRects().length) return;
        if (uiAnimations().some(a => a.playState !== 'finished' &&
            Number.isFinite(a.effect?.getComputedTiming().endTime))) {
            warmTimer = setTimeout(warmHistory, 150);
            return;
        }
        page('older');
    }
    element.addEventListener('scroll', () => {
        if ($('chat-workspace').inert || !element.getClientRects().length) return;
        if (scrollMotion) {
            lastTop = element.scrollTop;
            // Older WebKit versions lack scrollend. One trailing fallback also
            // bounds a lost event, without polling or a permanent animation loop.
            clearTimeout(scrollTimer);
            scrollTimer = setTimeout(() => finishScroll(true),180);
            navigation();
            return;
        }
        const top = element.scrollTop, delta = top - lastTop;
        if (delta < 0 && element.scrollHeight - top - element.clientHeight > 2)
            streamingFollow = false;
        const time = performance.now();
        velocity = .6 * velocity + .4 * Math.abs(delta) / Math.max(16, time - lastScroll);
        lastScroll = time;
        lastTop = top;
        stablePoint = point();
        const buffer = Math.min(element.clientHeight * 4,
                                Math.max(element.clientHeight * 1.5, velocity * latency * 2));
        if (delta < 0 && top < buffer)
            page('older');
        if (delta > 0 && element.scrollHeight - top - element.clientHeight < buffer)
            page('newer');
        if (delta > 0 && element.scrollHeight - top - element.clientHeight < 2)
            streamingFollow = true;
        navigation();
    }, {passive : true});
    element.addEventListener('wheel', e => {
        stopScroll();
        if (e.deltaY < 0) {
            streamingFollow = false;
            $('latest').hidden = false;
            if (element.scrollTop < 10)
                page('older');
        }
    }, {passive : true});
    element.addEventListener('keydown', e => {
        if (['PageUp','Home','ArrowUp'].includes(e.key)) {
            stopScroll();
            streamingFollow = false;
            $('latest').hidden = false;
            if (e.key === 'Home' && !e.target.closest('input,textarea,[contenteditable=true]')) {
                e.preventDefault();
                scrollToEdge(false);
            }
        }
    });
    function progress(update, change) {
        const turn = turns.find(t => t.id === update.id);
        if (turn) {
            turn.answer = update.answer || '';
            turn.reasoning = update.reasoning || '';
        }
        const saved = streamingFollow ? null : point();
        change();
        if (!scrollMotion)
            restorePosition(saved, streamingFollow);
        stablePoint = point();
        navigation();
        if (performance.now() - lastReport > 250) report();
    }
    return {receive, rendered, page, reflow, progress, suspend:stopScroll,
            moving:() => !!scrollMotion,
            following : () => streamingFollow,
            resumeAtEnd : () => {
                if (!newer && element.getClientRects().length &&
                    element.scrollHeight - element.clientHeight - element.scrollTop <= 2)
                    streamingFollow = true;
            },
            resumeGeneration : () => {
                if (!current?.active || current.activeChat !== current.selected) return;
                stopScroll();
                streamingFollow = true;
                if (newer) page('latest');
                else restorePosition(null, true);
                for (const body of element.querySelectorAll('.reasoning[data-pending=true] .reasoning-body')) {
                    body.reasoningFollow = true;
                    body.scrollTop = body.scrollHeight;
                }
                navigation();
            },
            settle : () => {
                if (!scrollMotion && element.getClientRects().length) {
                    restorePosition(streamingFollow ? null : stablePoint, streamingFollow);
                    // A wrap/font change can resize content without scrolling the
                    // viewport. Refresh fades, tail navigation and inspection too.
                    navigation();
                }
            }, submitting : () => {
                stopScroll();
                streamingFollow = !newer && element.scrollHeight - element.scrollTop - element.clientHeight <= 2;
                if (streamingFollow) pending = null;
                return streamingFollow;
            },
            jump : (chat, id) => { jump = id ? {chat, id} : null; }};
})();
