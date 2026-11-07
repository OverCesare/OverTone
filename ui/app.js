'use strict';
let submissionSequence = 0, navigationRevision = 0, draftRevision = 0, pendingSubmission = null, acceptedNewChat = null;
const send = value => {
    if (value.command === 'select') navigationRevision++;
    if (value.command === 'send' || value.command === 'retry') {
        Object.assign(value, {request: ++submissionSequence, follow: window.transcriptView?.submitting(), chat: current?.selected || 0});
        pendingSubmission = {request: value.request, chat: value.chat, replace: value.replace || 0,
                             prompt: value.prompt, navigation: navigationRevision, draft: draftRevision};
    }
    window.webkit.messageHandlers.overtone.postMessage(JSON.stringify(value));
};
const make = (tag, text = '', className = '') => {
    const n = document.createElement(tag);
    if (text)
        n.textContent = text;
    if (className)
        n.className = className;
    return n
};
function modelLabel(model) {
    if (!model) return '';
    const name=model.displayName || model.metadata?.modelName || model.name || model.title || '',
          quantization=model.quantization || model.metadata?.quantization || '',
          token=quantization.replace(/[.*+?^${}()|[\]\\]/g,'\\$&'),
          alreadyNamed=quantization && new RegExp('(?:^|[^a-z0-9])'+token+'(?:$|[^a-z0-9])','i').test(name);
    return [name,alreadyNamed ? '' : quantization].filter(Boolean).join(' · ');
}
function connectionLabel() {
    const c = current?.connection;
    return c ? modelLabel({name:c.displayName || c.model,quantization:c.quantization}) : '';
}
// Report code location only: diagnostics must not collect messages or user content.
onUIDocument(doc => doc.defaultView.addEventListener('error', event => {
    const file = event.filename.split('/').pop();
    if (/^[a-z-]+\.js$/.test(file))
        send({command: 'rendererFault', file, line: event.lineno, column: event.colno});
}));
function surfaceIsOpen(el) {
    return typeof el.showPopover === 'function' ? el.matches(':popover-open')
        : el.surfaceVisible ?? !el.hidden
}
function setMeter(track, fraction) {
    const known = Number.isFinite(fraction), value = known ? Math.max(0, Math.min(1, fraction)) : 0;
    track.setAttribute('aria-valuemin', '0');
    track.setAttribute('aria-valuemax', '100');
    if (known)
        track.setAttribute('aria-valuenow', String(Math.round(value * 10000) / 100));
    else
        track.removeAttribute('aria-valuenow');
    track.firstElementChild.style.transform = `scaleX(${value})`;
}
function icon(name) {
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('class', 'icon');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('aria-hidden', 'true');
    svg.setAttribute('focusable', 'false');
    const use = document.createElementNS(svg.namespaceURI, 'use');
    use.setAttribute('href', '#i-' + name);
    svg.append(use);
    return svg;
}
function actionButton(label, name, action, intent = '') {
    const b = button(label, action);
    b.className = 'icon-button' + (intent ? ' ' + intent : '');
    b.setAttribute('aria-label', tr(label));
    b.dataset.tip = tr(label);
    localAccessible(b, label);
    b.replaceChildren(icon(name));
    return b;
}
const brandMark = $('brand-mark');
// The first durable workspace projection is the readiness signal. Never delay it
// to finish a visual cycle: an early response exposes the approved static master.
const startupCover = $('startup-cover');
let startupPending = true, startupExit = null, startupTimer = 0;
function startStartup(replay = false) {
    clearTimeout(startupTimer);startupTimer=0;
    startupExit?.cancel();startupExit=null;startupPending=true;
    startupCover.classList.remove('show-logo');
    $('startup-emblem').getAnimations().forEach(effect => effect.cancel());
    startupCover.hidden=startupCover.inert=false;
    startupCover.style.pointerEvents='';
    document.documentElement.classList.add('startup-pending');
    document.documentElement.setAttribute('aria-busy','true');
    brandMark.classList.add('is-loading');
    send({command:'startup',mode:replay ? 'replay' : 'play'});
    if (document.documentElement.dataset.nativeLoading !== 'true') {
        const show=() => {startupTimer=0;if (startupPending) startupCover.classList.add('show-logo');};
        if (replay) show(); else startupTimer=setTimeout(show,1000);
    }
}
function finishStartup() {
    if (!startupPending) return;
    startupPending=false;clearTimeout(startupTimer);startupTimer=0;
    brandMark.classList.remove('is-loading');
    document.documentElement.classList.remove('startup-pending');
    document.documentElement.setAttribute('aria-busy','false');
    delete document.documentElement.dataset.transfer;
    startupCover.inert=true;startupCover.style.pointerEvents='none';
    const duration=motionEnabled() ? motionTiming('exit').duration : 0;
    send({command:'startup',mode:'finish',duration});
    if (document.documentElement.dataset.nativeLoading === 'true' || !duration || !startupCover.classList.contains('show-logo')) startupCover.hidden=true;
    else {
        const timing=motionTiming('exit');
        $('startup-emblem').animate([{opacity:1},{opacity:0}],{...timing,duration:timing.duration/2,fill:'forwards'});
        const effect=startupExit=startupCover.animate([{opacity:1,offset:0},{opacity:1,offset:.5},{opacity:0,offset:1}],timing);
        effect.finished.then(() => {
            if (startupExit !== effect) return;
            startupExit=null;startupCover.hidden=true;
        },() => {});
    }
}
function replayStartup() {if (!startupPending) startStartup(true);}

function flushDraft() {
    clearTimeout(draftTimer);
    if (!editing && current)
        send({command : 'draft', chat : current.selected, text : $('prompt').value});
}
function showSuggestion() {
    $('improve').setAttribute('aria-expanded','true');
    $('improve').setAttribute('aria-pressed','true');
    setSurfaceVisible($('suggestion-panel'), true, {collapse:true});
    controls();
}
function hideSuggestion(immediate = false) {
    $('improve').setAttribute('aria-expanded','false');
    $('improve').setAttribute('aria-pressed','false');
    const panel = $('suggestion-panel');
    if (!immediate) {
        setSurfaceVisible(panel, false, {collapse:true});
        return;
    }
    finishSuggestionMotion();
    panel.surfaceAnimation?.cancel();
    panel.surfaceAnimation = null;
    panel.surfaceVisible = false;
    panel.hidden = panel.inert = true;
    $('prompt').hidden = false;
}
let current = null, waiting = false, conversation = 0, editing = 0, draftBeforeEdit = '', seenChatReveal = 0;
let preferenceSignature = '';
let searchTimer, draftTimer, improvementRequest = null;
const rows = new Map();
const explanations = {
    complete : 'Il modello non ha restituito una risposta. Riprova.',
    failed : 'Il modello non ha completato la risposta. Riprova.',
    cancelled : 'Generazione interrotta.',
    interrupted : 'La generazione è stata interrotta dalla chiusura dell’app.',
    connection_failed : 'Connessione al motore non riuscita. Controlla server e modello.',
    incomplete_stream : 'Il motore ha chiuso la risposta prima del completamento.',
    empty_response : 'Il modello non ha restituito una risposta. Riprova.',
    output_limit : 'Limite di output raggiunto.',
    content_filter : 'Limitata dal servizio',
    tool_calls : 'Strumenti richiesti',
    repetition : 'Generazione interrotta: il modello ripeteva lo stesso testo.',
    invalid_stream : 'Risposta del motore non valida o troppo grande.',
    stream_limit : 'Il flusso supera il limite consentito. Il testo ricevuto è conservato.',
    storage_write : 'Salvataggio non riuscito. Copia il testo disponibile prima di chiudere.',
    storage_read : 'Lettura dei dati non riuscita. Riprova o riapri la conversazione.',
    invalid_input : 'Controlla il testo inserito e il modello selezionato.',
    invalid_turn : 'La versione richiesta non è disponibile in questa conversazione.',
    file_operation : 'Operazione sui file non riuscita.',
    busy : 'Attendi il termine della generazione.',
    request_failed : 'Impossibile preparare la richiesta.',
    model_unavailable : 'Seleziona un modello e attendi che sia pronto.',
    chat_read_only : 'Ripristina la conversazione per scrivere.',
    improvement_disabled : 'Il miglioramento del prompt è disattivato nelle impostazioni.',
    context_limit : 'Il messaggio supera il contesto configurato. Aumenta il contesto o riduci il testo.'
};
function explain(code) {
    const http = /^http_([1-5][0-9]{2})$/.exec(code || '');
    return tr(explanations[code] || window.modelErrorText?.(code) || window.connectionErrorText?.(code) ||
              (http ? tr('Il server ha risposto con errore {code}.').replace('{code}',http[1])
                    : 'Operazione non riuscita.'));
}
function button(label, action, literal = false) {
    const b = document.createElement('button');
    b.type = 'button';
    if (literal) b.textContent = label;
    else b.append(localLabel('span', label));
    b.onclick = action;
    return b;
}
function copyButton(text) {
    const b = actionButton('Copia', 'copy', () => {
        const value = b.copyValue;
        send({command : 'copy', text : typeof value === 'function' ? value() : value});
        b.replaceChildren(icon('check'));
        b.setAttribute('aria-label', tr('Copiato'));
        b.dataset.tip = tr('Copiato');
        clearTimeout(b.copyTimer);
        b.copyTimer = setTimeout(() => {
            b.copyTimer = 0;
            if (!b.isConnected)
                return;
            b.replaceChildren(icon('copy'));
            b.setAttribute('aria-label', b.copyLabel || tr('Copia'));
            b.dataset.tip = b.getAttribute('aria-label');
        }, 1400);
    });
    b.copyValue = text;
    b.className = 'copy icon-button';
    return b;
}

function article(role) {
    const a = document.createElement('article');
    a.className = 'message ' + role;
    const header = document.createElement('header'), h = document.createElement('h3');
    h.append(icon(role === 'user' ? 'messages-square' : 'box'),
             document.createTextNode(tr(role === 'user' ? 'Utente' : 'Assistente')));
    const time = document.createElement('time');
    h.append(time);
    header.append(h);
    const body = document.createElement('div'), actions = document.createElement('footer');
    body.className = 'message-body';
    actions.className = 'turn-actions';
    a.append(header, body, actions);
    const row = {element : a, header, time, body, actions, text : null, role};
    row.copy = copyButton(() => row.text || '');
    return row;
}

function turnRow(turn) {
    const row = {user : article('user'), assistant : article('assistant')};
    row.edit = actionButton('Modifica', 'pencil', () => beginEdit(turn));
    row.user.actions.append(row.edit, row.user.copy);
    row.retry = actionButton('Rigenera', 'rotate-ccw', () => {
        if (!current.ready) {
            window.navigate('models');
            return;
        }
        endEdit(true);
        waiting = true;
        controls();
        send({command : 'retry', replace : turn.id});
    });
    const fork = actionButton('Crea un ramo · non ancora disponibile', 'git-branch', () => {});
    const knowledge = actionButton('Aggiungi a Documenti · non ancora disponibile', 'library', () => {});
    fork.disabled = knowledge.disabled = true;
    row.assistant.actions.append(row.retry, fork, knowledge, row.assistant.copy);
    return row;
}

function updateArticle(row, text) {
    if (row.text === text)
        return;
    row.text = text;
    if (row.role === 'user')
        row.body.replaceChildren(textSpan(text));
    else {
        const documents = [], lines = [], marker = /^\[overtone-generated-document\|([^|\]\n]{1,200})\|([^|\]\n]{1,80})\|([^|\]\n]{1,80})\|([^|\]\n]{1,1000})\]$/;
        let fenced = false;
        for (const line of text.split('\n')) {
            if (/^\s*(```|~~~)/.test(line))
                fenced = !fenced;
            const match = !fenced && documents.length < 32 ? line.match(marker) : null;
            if (match)
                documents.push(match.slice(1).map(field => field.trim()));
            else
                lines.push(line);
        }
        renderMarkdown(row.body, lines.join('\n'));
        let cards = row.element.querySelector('.generated-documents');
        if (!documents.length)
            cards?.remove();
        else {
            if (!cards) {
                cards = document.createElement('div');
                cards.className = 'generated-documents';
                row.body.after(cards);
            }
            const signature = JSON.stringify([documents, language]);
            if (cards.signature !== signature) {
                cards.signature = signature;
                cards.replaceChildren();
                for (const [title, type, size, description] of documents) {
                    const card = document.createElement('article'), heading = document.createElement('h3'),
                          info = document.createElement('small'), summary = document.createElement('p'),
                          add = actionButton('Aggiungi a Documenti · non ancora disponibile', 'library', () => {});
                    heading.append(icon('file-text'), textSpan(title));
                    info.append(textSpan(type + ' / ' + size));
                    summary.append(textSpan(description));
                    add.disabled = true;
                    card.append(heading, info, summary, add);
                    cards.append(card);
                }
            }
        }
    }
    row.copy.copyLabel = tr(row.role === 'user' ? 'Copia messaggio' : 'Copia risposta');
    if (!row.copy.copyTimer && row.copy.getAttribute('aria-label') !== row.copy.copyLabel) {
        row.copy.setAttribute('aria-label', row.copy.copyLabel);
        row.copy.dataset.tip = row.copy.copyLabel;
    }
}

function typing(row, active) {
    let dots = row.element.querySelector('.typing-dots');
    if (!active) {
        dots?.remove();
        return;
    }
    if (!dots) {
        dots = document.createElement('span');
        dots.className = 'typing-dots';
        dots.setAttribute('role', 'status');
        dots.setAttribute('aria-label', tr('Generazione in corso'));
        for (let i = 0; i < 3; i++)
            dots.append(document.createElement('i'));
        row.body.after(dots);
    }
}
const reasoningResize = new ResizeObserver(entries => {
    for (const {target} of entries) {
        const body = target.querySelector('.reasoning-body');
        if (target.dataset.pending === 'true' && body?.reasoningFollow)
            body.scrollTop = body.scrollHeight;
    }
    window.transcriptView.settle();
});
function reasoning(row, turn) {
    const text = turn.reasoning, pending = turn.state === 'pending';
    let panel = row.element.querySelector('.reasoning');
    if (!text) {
        if (panel)
            reasoningResize.unobserve(panel);
        panel?.remove();
        return;
    }
    if (!panel) {
        panel = document.createElement('section');
        panel.className = 'reasoning response-activity';
        const header = document.createElement('div'), content = document.createElement('div'),
              caption = document.createElement('span');
        header.className = 'reasoning-heading';
        caption.id = 'reasoning-status-' + turn.id;
        caption.setAttribute('role', 'status');
        caption.setAttribute('aria-atomic', 'true');
        content.className = 'reasoning-body';
        content.id = 'reasoning-' + turn.id;
        content.hidden = true;
        content.reasoningFollow = true;
        content.addEventListener('wheel', event => {
            if (event.deltaY < 0) content.reasoningFollow = false;
        }, {passive:true});
        content.addEventListener('scroll', () => {
            if (!content.getClientRects().length) return;
            content.reasoningFollow = content.scrollHeight - content.clientHeight - content.scrollTop <= 2;
            if (content.reasoningFollow) window.transcriptView.resumeAtEnd();
        }, {passive:true});
        const toggle = actionButton('Mostra ragionamento', 'brain', () => {
            panel.manual = true;
            window.transcriptView.reflow(
                () => panel.setOpen(toggle.getAttribute('aria-expanded') !== 'true'), true);
        });
        toggle.className = 'reasoning-toggle';
        delete toggle.dataset.tip;
        toggle.append(caption, icon('chevron-right'));
        toggle.setAttribute('aria-expanded', 'false');
        toggle.setAttribute('aria-controls', content.id);
        toggle.setAttribute('aria-describedby', caption.id);
        const copy = copyButton(() => content.reasoningText || '');
        copy.copyLabel = tr('Copia ragionamento');
        copy.setAttribute('aria-label', copy.copyLabel);
        copy.dataset.tip = copy.copyLabel;
        copy.hidden = true;
        panel.setOpen = open => {
            if (open && content.hidden)
                renderMarkdown(content, content.reasoningText || '');
            reasoningResize.observe(panel);
            const finished = setSurfaceVisible(content, open, {collapse:true});
            if (!open)
                finished?.then(() => reasoningResize.unobserve(panel), () => {});
            toggle.setAttribute('aria-expanded', String(open));
            const label = tr(open ? 'Nascondi ragionamento' : 'Mostra ragionamento');
            toggle.setAttribute('aria-label', label);
            delete toggle.dataset.tip;
            copy.hidden = !open;
        };
        header.append(toggle, copy);
        panel.append(header, content);
        revealContainers(content);
        row.body.before(panel);
        // Historical panels arrive with the conversation. Animating each one
        // would force layout between successive message writes during loading.
        if (pending) {
            if (panel.isConnected) enterSurface(panel);
            else row.activityEntrance = panel;
        }
    }
    const body = panel.querySelector('.reasoning-body'), toggle = panel.querySelector('.reasoning-toggle'),
          thinking = pending && !turn.answer;
    panel.dataset.pending = String(thinking);
    const mode = turn.completion?.requestedThinking;
    const label = thinking ? tr('Ragionamento in corso') :
        tr('Ragionamento') + (mode && !['auto','on','off','none'].includes(mode) ? ' · ' + thinkingLabel(mode) : '') +
        (turn.completion?.thinkingMs > 0 ? ' · ' +
            decimalFormat.format(turn.completion.thinkingMs / 1000) + ' s' : '');
    const caption = toggle.querySelector('span');
    if (caption.textContent !== label)
        caption.textContent = label;
    if (body.reasoningText !== text) {
        const top = body.reasoningFollow ? 0 : body.scrollTop;
        body.reasoningText = text;
        if (toggle.getAttribute('aria-expanded') === 'true') {
            renderMarkdown(body, text);
            if (!body.reasoningFollow) body.scrollTop = top;
        }
    }
    if (!panel.manual && toggle.getAttribute('aria-expanded') !== String(thinking))
        panel.setOpen(thinking);
    if (pending && body.reasoningFollow && toggle.getAttribute('aria-expanded') === 'true')
        body.scrollTop = body.scrollHeight;
}
function completionMetadata(row, turn) {
    const metrics = turn.completion, signature = JSON.stringify([ metrics, language, formatLocale, !!turn.answer ]);
    if (row.completionSignature === signature)
        return;
    row.completionSignature = signature;
    row.element.querySelector('.response-metadata')?.remove();
    if (!turn.answer || !metrics?.durationMs)
        return;
    const list = document.createElement('div');
    list.className = 'response-metadata';
    const rate =
        metrics.outputTokens > 0 && metrics.evalMs > 0 ? metrics.outputTokens / (metrics.evalMs / 1000) : 0;
    for (const [glyph, label, value] of [
             [
                 'clock-3', 'Tempo risposta',
                 decimalFormat.format(metrics.durationMs / 1000) + ' s'
             ],
             [
                 'arrow-down', 'Token di input',
                 metrics.inputTokens > 0 ? countFormat.format(metrics.inputTokens) : ''
             ],
             [
                 'arrow-up', 'Token di output',
                 metrics.outputTokens > 0 ? countFormat.format(metrics.outputTokens) + ' token' : ''
             ],
             [
                 'gauge', 'Velocità risposta',
                 rate > 0 ? decimalFormat.format(rate) + ' token/s' : ''
             ]]) {
        if (!value)
            continue;
        const item = document.createElement('span');
        item.dataset.tip = tr(label);
        const caption = document.createElement('span');
        caption.className = 'sr-only';
        caption.textContent = tr(label) + ': ';
        item.append(icon(glyph), caption, document.createTextNode(value));
        list.append(item);
    }
    row.actions.append(list);
}

function responseStatus(row, turn, retry) {
    const terminal = turn.completion?.stopReason;
    const code = turn.error || (['content_filter','tool_calls'].includes(terminal) ? terminal :
        turn.state !== 'pending' && !turn.answer ? turn.state : '');
    let status = row.element.querySelector('.response-status');
    const signature = [code, !!turn.answer, language].join(':');
    if (status?.signature === signature) return;
    // A retry is one action, even when it moves into an empty-response status.
    if (retry.parentElement !== row.actions)
        row.actions.prepend(retry);
    if (!code) {
        status?.remove();
        return;
    }
    if (!status) {
        status = make('aside', '', 'response-status');
        status.setAttribute('role', 'status');
        status.setAttribute('aria-label', tr('Stato di OverTone'));
        row.body.after(status);
    }
    status.signature = signature;
    setFeedback(status,explain(code),feedbackKind(code));
    if (!turn.answer) status.append(retry);
}
function controls() {
    const browsingLocked = !current || current.maintenance || current.storageFailed;
    const readOnly = !!(current?.archived || current?.deleted);
    const locked = browsingLocked || waiting || !!improvementRequest?.pending || current.active || current.improving ||
                   current.storageFailed;
    const visibleDraft = $('improve').getAttribute('aria-expanded') === 'true' ? $('suggestion') : $('prompt');
    $('send').disabled = locked || readOnly || !current?.ready || !!editing || !visibleDraft.value.trim();
    for (const row of rows.values()) row.retry.disabled = !!(locked || readOnly);
    $('prompt').disabled = !current || readOnly || !!editing;
    $('composer').hidden = readOnly;
    if (readOnly) closeComposerMenu();
    $('chat-readonly').hidden = !readOnly;
    if (readOnly) {
        const message = tr(current.deleted ? 'Chat nel cestino. Ripristina per scrivere.'
                                           : 'Chat archiviata. Ripristina per scrivere.');
        if ($('readonly-message').textContent !== message)
            $('readonly-message').textContent = message;
        $('chat-readonly').querySelector('use').setAttribute('href', current.deleted ? '#i-trash' : '#i-archive');
    }
    $('restore-readonly').disabled = !!browsingLocked;
    $('stop-readonly').hidden = !(readOnly && (current.active || current.improving));
    $('history-actions').hidden = !current?.filter;
    $('empty-trash').hidden = current?.filter !== 2;
    $('restore-archive').hidden = current?.filter !== 1;
    $('empty-trash').disabled = !!browsingLocked || !current?.trashTotal;
    $('restore-archive').disabled = !!browsingLocked || !current?.archiveTotal;
    $('stop').hidden = !current?.active && !current?.improving;
    $('send').hidden = !$('stop').hidden;
    const here = current?.active && current.activeChat === current.selected &&
                 rows.has(current.activeTurn);
    $('stream-activity').hidden = !here;
    $('latest').querySelector('svg').toggleAttribute('hidden', !!here);
    $('restore').disabled = Boolean(locked);
    for (const id of ['new', 'configure', 'chat-title', 'archive-chat', 'trash-chat',
                      'restore-chat', 'delete-chat', 'backup', 'export'])
        $(id).disabled = Boolean(browsingLocked);
    for (const control of document.querySelectorAll('[data-requires-idle]'))
        control.disabled = !!locked;
    $('preferences').disabled = !current || current.storageFailed;
    $('chat-title').disabled = browsingLocked || !current?.selected;
    $('archive-chat').hidden = !current?.selected || current?.archived;
    $('delete-chat').hidden = !current?.selected;
    $('trash-chat').hidden = !current?.selected;
    $('restore-chat').hidden = !current?.selected || (!current.deleted && !current.archived);
    $('move-project').hidden = !current?.selected;
    const review = !!improvementRequest && improvementRequest.chat === current?.selected && improvementRequest.source === $('prompt').value &&
                   !improvementRequest.dismissed && !!improvementRequest.result;
    window.improveFeedback?.update(!!improvementRequest?.pending);
    $('improve').disabled = browsingLocked || readOnly || !!editing || !!improvementRequest?.pending ||
        (!review && (locked || !current?.ready || !current?.preferences.improve || !$('prompt').value.trim() || current?.auxiliary));
    $('improve').setAttribute('aria-busy', String(!!improvementRequest?.pending));
    localAccessible($('improve'),improvementRequest?.pending ? 'Miglioramento del prompt' : 'Migliora prompt');
    $('accept-suggestion').disabled = locked || readOnly || !$('suggestion').value.trim();
    if (review) {
        $('original-prompt').parentElement.hidden = improvementRequest.source === $('suggestion').value;
        $('suggestion-warning').hidden = !$('suggestion-warning').textContent || improvementRequest.result !== $('suggestion').value;
    }
    const trashLabel = tr(current?.deleted ? 'Elimina definitivamente' : 'Sposta nel cestino');
    $('trash-chat').setAttribute('aria-label', trashLabel);
    $('trash-chat').dataset.tip = trashLabel;
    $('trash-chat').setAttribute('aria-haspopup', current?.deleted ? 'dialog' : 'menu');
    $('trash-chat').setAttribute('aria-controls', current?.deleted ? 'action-dialog' : 'chat-removal');
    if (current?.deleted) $('trash-chat').removeAttribute('popovertarget');
    else $('trash-chat').setAttribute('popovertarget', 'chat-removal');
    $('send').setAttribute('aria-label', tr('Invia messaggio'));
    window.updateComposer?.();
    window.updateAppMenu?.();
}
function endEdit(restore) {
    if (editing && restore)
        $('prompt').value = draftBeforeEdit;
    const reset = () => {
        for (const row of rows.values()) {
            row.user.element.querySelector('.inline-editor')?.remove();
            row.user.body.hidden = false;
        }
    };
    if (editing) window.transcriptView.reflow(reset);
    else reset();
    editing = 0;
    draftBeforeEdit = '';
    controls();
}

function beginEdit(turn) {
    if (waiting || current?.archived || current?.deleted || current?.maintenance || current?.storageFailed)
        return;
    flushDraft();
    endEdit(true);
    draftBeforeEdit = $('prompt').value;
    editing = turn.id;
    const row = rows.get(turn.id);
    if (!row) {
        endEdit(true);
        return;
    }
    const editor = document.createElement('div');
    editor.className = 'inline-editor';
    const input = document.createElement('textarea');
    input.value = turn.prompt;
    input.oninput = () => { draftRevision++; };
    input.maxLength = 16384;
    input.setAttribute('aria-label', tr('Modifica messaggio'));
    const actions = document.createElement('div');
    actions.className = 'row';
    const cancel = button('Annulla', () => {
        endEdit(true);
        row.user.actions.querySelector('button')?.focus()
    });
    const submit = button('Salva e invia', () => {
        if (!input.value.trim() || input.value.length > 16384 || waiting || current.active ||
            current.storageFailed || current.maintenance)
            return;
        if (!current.ready) {
            window.navigate('models');
            return;
        }
        waiting = true;
        submit.disabled = true;
        cancel.disabled = true;
        controls();
        send({command : 'send', prompt : input.value, replace : turn.id});
    });
    submit.className = 'primary';
    submit.dataset.requiresIdle = 'true';
    actions.append(cancel, submit);
    editor.append(input, actions);
    input.onkeydown = e => {
        if (e.key === 'Escape') {
            e.preventDefault();
            cancel.click()
        } else if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) {
            e.preventDefault();
            submit.click()
        }
    };
    window.transcriptView.reflow(() => {
        row.user.body.hidden = true;
        row.user.body.after(editor);
    });
    applyWritingPreferences(current.preferences);
    controls();
    input.focus();
}

function applyWritingPreferences(p) {
    if (!p.tooltips)
        hideTip();
    document.documentElement.dataset.codeWrap = String(!!p.codeWrap);
    for (const input of uiElements('#prompt,#suggestion,.inline-editor textarea')) {
        input.spellcheck = !!p.spellcheck;
        if (p.inputLanguage && p.inputLanguage !== 'auto')
            input.lang = p.inputLanguage;
        else
            input.removeAttribute('lang');
    }
    $('improve-context-count').textContent = String(p.improveContext);

}
function grantNetwork(scopes) {
    if (!scopes) return;
    const p=current.preferences;
    send({command:'preferences',value:{...p,internet:true,network:p.internet ? p.network | scopes : scopes}});
}
function networkAllowed(scope, p = current?.preferences) {
    return scope === 0 || !!p?.internet && ((p.network ?? 15) & scope) === scope;
}
function syncNetworkPreferences(p) {
    for (const input of document.querySelectorAll('[data-network]')) {
        input.checked = !!((p.network ?? 15) & Number(input.dataset.network));
        input.disabled = !p.internet;
    }
    const limited=!!p.internet && p.network!==15;
    $('network-state').hidden=!limited;
    $('network-state').textContent=limited ? tr('Accesso limitato') : '';
}
function fillPreferenceFields(p, preserveFocus = false) {
    for (const field of document.querySelectorAll('[data-pref]')) {
        if (preserveFocus && field === document.activeElement)
            continue;
        field[field.type === 'checkbox' ? 'checked' : 'value'] = p[field.dataset.pref];
    }
    syncNetworkPreferences(p);
    regionalPreview();
}
window.overtoneError = (message, fatal = false, scope = '') => {
    finishStartup();
    if (!fatal && current?.storageFailed && fatalFeedbackMessage) {
        refreshFatalFeedback();
        return;
    }
    if (fatal) fatalFeedbackMessage = message;
    if (fatal && current)
        current.storageFailed = true;
    const target = feedbackTarget(scope);
    target.feedbackFatal = fatal;
    setFeedback(target,message);
    target.feedbackChat = fatal ? null : current?.selected;
    waiting = false;
    if (target.id === 'settings-error')
        $('save-settings').disabled = false;
    if (target.id === 'model-parameters-error') window.modelParametersFailed?.();
    controls();
    if (editing) {
        const row = rows.get(editing);
        for (const b of row?.user.element.querySelectorAll('.inline-editor button') || [])
            b.disabled = !!fatal;
    }
    window.failPendingChatAction?.();
};
window.overtoneConfigured = () => {
    waiting = false;
    $('save-settings').disabled = false;
    window.configurationServerReady?.();
    closeDialog($('settings'));
    controls();
};
window.overtoneAccepted = result => {
    const submitted = pendingSubmission;
    if (!submitted || result.request !== submitted.request) return;
    pendingSubmission = null;
    waiting = false;
    const sameChat = submitted.navigation === navigationRevision &&
        (current?.selected === submitted.chat || !submitted.chat && current?.selected === result.chat);
    const unchanged = sameChat && submitted.draft === draftRevision;
    if (result.accepted && !submitted.chat && sameChat)
        acceptedNewChat = {chat: result.chat, navigation: navigationRevision};
    if (result.accepted && submitted.prompt !== undefined && unchanged) {
        if (submitted.replace && editing === submitted.replace) {
            const input = rows.get(editing)?.user.element.querySelector('.inline-editor textarea');
            if (input?.value === submitted.prompt) { endEdit(true); flushDraft(); }
        } else if (!submitted.replace && !editing && $('prompt').value === submitted.prompt) {
            clearTimeout(draftTimer);
            $('prompt').value = '';
        }
    }
    if (!result.accepted && sameChat && editing === submitted.replace)
        for (const b of rows.get(editing)?.user.element.querySelectorAll('.inline-editor button') || []) b.disabled = false;
    // An acknowledgement must not steal focus from another chat, editor or dialog.
    if (sameChat && route === 'chat' && !document.querySelector('dialog[open]') &&
        ['prompt','send'].includes(document.activeElement?.id)) $('prompt').focus({preventScroll:true});
    controls();
};
let deferredProgress = null, progressFrame = 0;
window.overtoneProgress = progress => {
    if (progress.chat !== current?.selected || !current.active || current.activeChat !== progress.chat)
        return;
    // Projections contain the cumulative text. Paint the latest once per frame,
    // rather than reshaping several intermediate versions before one presentation.
    deferredProgress = progress;
    if (route === 'chat' && !document.hidden && !progressFrame)
        progressFrame = requestAnimationFrame(window.flushChatProgress);
};
window.flushChatProgress = () => {
    cancelAnimationFrame(progressFrame);
    progressFrame = 0;
    if (!deferredProgress || route !== 'chat' || document.hidden) return;
    const progress = deferredProgress;
    deferredProgress = null;
    if (progress.chat !== current?.selected || !current.active || current.activeChat !== progress.chat)
        return;
    const row = rows.get(progress.id);
    if (!row)
        return;
    const started = performance.now();
    window.transcriptView.progress(progress, () => {
        updateArticle(row.assistant, progress.answer || '');
        row.assistant.copy.disabled = !progress.answer;
        row.assistant.actions.hidden = !progress.answer;
        typing(row.assistant, !progress.answer && !progress.reasoning);
        reasoning(row.assistant, {...progress, state : 'pending'});
    });
    recordMotionWork('update',started);
};
window.overtoneReceive = state => {
    const started = performance.now();
    deferredProgress = null; // A full C projection supersedes any deferred fragment.
    cancelAnimationFrame(progressFrame);
    progressFrame = 0;
    const previousChat = current?.selected;
    const reveal = !!state.chatRevealRevision && state.chatRevealRevision !== seenChatReveal;
    if (reveal) {
        seenChatReveal = state.chatRevealRevision;
        endEdit(true);
        $('search').value = '';
        $('clear-search').hidden = true;
        resetChatFilters(false);
        window.resetConversationWindow();
    }
    current = state;
    const regionalChanged = setRegionalPreferences(state.preferences);
    const languageChanged = setLanguage(state.preferences.language);
    if (regionalChanged) {
        refreshTimestamps();
        window.refreshLibrary?.();
        regionalPreview();
    }
    $('chat-title').replaceChildren(make('span', state.selected ? state.title : tr('Chat')), ...(state.selected ? [icon('pencil')] : []));
    $('chat-title').removeAttribute('aria-label');
    if (state.selected) {
        $('chat-title').dataset.tip = tr('Rinomina titolo');
        $('chat-title').setAttribute('aria-description', tr('Rinomina titolo'));
    } else {
        delete $('chat-title').dataset.tip;
        $('chat-title').removeAttribute('aria-description');
    }
    waiting = !!pendingSubmission;
    const display = window.transcriptView.receive(state, previousChat);
    if (languageChanged) {
        reasoningResize.disconnect();
        rows.clear();
        $('transcript').replaceChildren();
        $('chats').dataset.signature = '';
    }
    if (previousChat !== state.selected) {
        endEdit(true);
        const keepDraft = !previousChat && acceptedNewChat?.chat === state.selected &&
                          acceptedNewChat.navigation === navigationRevision;
        if (keepDraft) flushDraft(); else $('prompt').value = state.draft || '';
        acceptedNewChat = null;
        hideSuggestion(true);
        improvementRequest = null;
    }
    const p = state.preferences;
    const nextPreferences = JSON.stringify(p);
    if (nextPreferences !== preferenceSignature) {
        preferenceSignature = nextPreferences;
        if ($('preferences-dialog').open) {
            fillPreferenceFields(p, true);
            window.syncPreferenceChoices?.();
        }
    }
    applyWritingPreferences(p);
    document.documentElement.dataset.theme = p.theme;
    document.documentElement.dataset.accent = p.accent;
    const historyGroup = $('history-filter');
    const updateHistory = () => {
        for (const b of historyGroup.querySelectorAll('[data-history-view]')) {
            const selected = Number(b.dataset.historyView) === state.filter;
            b.setAttribute('aria-selected', String(selected));
            b.tabIndex = selected ? 0 : -1;
        }
    };
    if (historyGroup.querySelector('[aria-selected=true]')?.dataset.historyView !== String(state.filter))
        window.selectionChange(historyGroup, updateHistory, '[aria-selected=true]');
    $('chats').setAttribute('aria-labelledby', 'history-view-' + state.filter);
    const revision = improvementRequest;
    if (revision && (state.archived || state.deleted || !state.preferences.improve || revision.head !== state.head ||
        revision.model !== state.connection.model || revision.endpoint !== state.connection.endpoint)) {
        revision.dismissed = true;hideSuggestion();
    }
    if (revision?.pending) {
        const started = state.suggestionId > revision.after;
        if (started && state.improving && revision.dismissed && !revision.cancelSent) {
            revision.cancelSent = true;
            send({command:'stop'});
        }
        if (started && !state.improving || !started && state.errorRevision > revision.error) {
            revision.pending = false;
            if (started && state.suggestion && !revision.dismissed && revision.chat === state.selected &&
                revision.source === $('prompt').value) {
                revision.result = state.suggestion;
                $('suggestion').value = state.suggestion;
                $('original-prompt').parentElement.hidden = revision.source === state.suggestion;
                const warning = $('suggestion-warning');
                warning.hidden = !state.suggestionLimited && !state.suggestionClamped && !state.suggestionRejected && state.suggestionChanges !== 0;
                warning.textContent = state.suggestionRejected
                    ? tr('Revisione non applicata. Il testo originale è conservato.')
                    : state.suggestionLimited ? tr('Il prompt potrebbe essere incompleto. Controllalo prima di usarlo.')
                    : state.suggestionClamped ? tr('Revisione limitata dal contesto disponibile.')
                    : state.suggestionChanges === 0 ? tr('Nessuna modifica proposta.') : '';
                if (!warning.hidden) warning.prepend(icon(state.suggestionRejected || state.suggestionLimited ? 'triangle-alert' : 'info'));
                if (route === 'chat' && !document.querySelector('dialog[open]')) {
                    showSuggestion();
                    if (!document.hidden) $('suggestion').focus({preventScroll:true});
                }
            }
        }
    }
    window.renderChatFilterOptions(state);
    window.renderConversationList(state, reveal);
    const transcript = $('transcript');
    if (conversation !== state.selected || !display.turns.length) {
        reasoningResize.disconnect();
        transcript.replaceChildren();
        rows.clear();
        conversation = state.selected;
    }
    if (!display.turns.length) {
        const p = document.createElement('p');
        p.className = 'empty';
        p.textContent = tr(display.loading ? 'Caricamento' : 'Da dove cominciamo?');
        transcript.append(p);
    }
    const ids = new Set(display.turns.map(t => t.id));
    for (const [id, row] of rows)
        if (!ids.has(id)) {
            const panel = row.assistant.element.querySelector('.reasoning');
            if (panel)
                reasoningResize.unobserve(panel);
            row.user.element.remove();
            row.assistant.element.remove();
            rows.delete(id);
        }
    let position = transcript.firstElementChild;
    function place(element) {
        if (element === position)
            position = position.nextElementSibling;
        else
            transcript.insertBefore(element, position);
    }
    for (const turn of display.turns) {
        let row = rows.get(turn.id);
        if (!row) {
            transcript.querySelector('.empty')?.remove();
            if (position?.classList.contains('empty'))
                position = null;
            row = turnRow(turn);
            rows.set(turn.id, row);
        }
        setTimestamp(row.user.time, turn.created);
        // A reply arrives when generation finishes, not when its question was sent.
        // Older test messages may have no receive time; retain their recorded time.
        row.assistant.time.hidden = turn.state === 'pending';
        setTimestamp(row.assistant.time, turn.completion?.receivedAt || turn.created);
        for (const part of [row.user, row.assistant]) {
            part.element.dataset.turn = turn.id;
            part.element.dataset.role = part.role;
        }
        updateArticle(row.user, turn.prompt);
        reasoning(row.assistant, turn);
        updateArticle(row.assistant, turn.answer || '');
        typing(row.assistant, turn.state === 'pending' && !state.storageFailed && !turn.answer && !turn.reasoning);
        window.renderVariants(row.assistant, turn, state);
        window.renderVariants(row.user, turn, state);
        completionMetadata(row.assistant, turn);
        row.edit.disabled = !!(state.archived || state.deleted || state.maintenance || state.storageFailed);
        row.assistant.copy.disabled = !turn.answer;
        row.assistant.actions.hidden = !turn.answer;
        responseStatus(row.assistant, turn, row.retry);
        // Finish new messages off-document. Their controls, Markdown and metadata
        // must not invalidate the visible conversation between construction steps.
        place(row.user.element);
        place(row.assistant.element);
        if (row.assistant.activityEntrance) {
            enterSurface(row.assistant.activityEntrance);
            delete row.assistant.activityEntrance;
        }
    }
    syncCoreFeedback(state,display.turns,languageChanged);
    window.transcriptView.rendered();
    controls();
    window.updateShell?.();
    window.finishPendingChatAction?.(state);
    window.updateSetup?.();
    window.updateModelAdvice?.();
    finishStartup();
    recordMotionWork('update',started);
};
$('close-settings').onclick = () => closeDialog($('settings'));
$('new').onclick = () => {
    if (improvementRequest?.pending && current.improving) send({command:'stop'});
    improvementRequest = null;
    hideSuggestion(true);
    endEdit(true);
    flushDraft();
    send({command : 'select', id : 0});
    setHistoryOpen(false);
    $('prompt').focus();
};
for (const id of ['stop','stop-readonly']) $(id).onclick = () => {
    send({command : 'stop'});
    if (!$('prompt').disabled) $('prompt').focus();
};

$('composer').onsubmit = event => {
    event.preventDefault();
    if ($('send').disabled)
        return;
    clearTimeout(draftTimer);
    if (!current?.ready) {
        window.navigate('models');
        return;
    }
    if ($('improve').getAttribute('aria-expanded') === 'true' && !applySuggestion()) return;
    improvementRequest = null;
    hideSuggestion();
    waiting = true;
    controls();
    send({command : 'send', prompt : $('prompt').value, replace : editing});
};
$('prompt').oninput = () => {
    draftRevision++;
    if (improvementRequest && improvementRequest.source !== $('prompt').value) {
        improvementRequest.dismissed = true;
        hideSuggestion();
        if (improvementRequest.pending && current.improving && !improvementRequest.cancelSent) {
            improvementRequest.cancelSent = true;
            send({command:'stop'});
        }
    }
    controls();
    clearTimeout(draftTimer);
    if (!editing)
        draftTimer = setTimeout(flushDraft, 500);
};
function submitOnShortcut(event) {
    const shortcut = current?.preferences.shortcut || 'enter';
    const trigger = shortcut === 'ctrl_enter'    ? (event.ctrlKey || event.metaKey)
                    : shortcut === 'shift_enter' ? event.shiftKey
                                                 : !event.shiftKey;
    if (event.key === 'Enter' && trigger && !event.isComposing) {
        event.preventDefault();
        $('composer').requestSubmit();
    }
}
$('prompt').onkeydown = $('suggestion').onkeydown = submitOnShortcut;
function search(before = 0) {
    send({command : 'search', query : $('search').value, before});
}
$('search').oninput = () => {
    clearTimeout(searchTimer);
    $('clear-search').hidden = !$('search').value;
    window.resetConversationWindow?.();
    searchTimer = setTimeout(() => search(), 200);
};
$('clear-search').onclick = () => {
    $('search').value = '';
    $('search').dispatchEvent(new Event('input'));
    $('search').focus();
};
$('older').onclick = () => { window.transcriptView.page('older'); };
$('latest').onclick = () => { window.transcriptView.page('latest'); };
function openRename() {
    $('new-title').value = current.title;
    setFeedback($('rename-error'));
    openDialog($('rename-dialog'));
    $('new-title').select();
}
$('chat-title').onclick = openRename;
$('cancel-rename').onclick = () => closeDialog($('rename-dialog'));
$('rename-form').onsubmit = event => {
    event.preventDefault();
    send({command : 'rename', title : $('new-title').value.trim()});
};
controls();
$('prompt').focus();

$('preferences').onclick = () => {
    fillPreferenceFields(current.preferences);
    setFeedback($('preferences-error'));
    openDialog($('preferences-dialog'));
};
$('close-preferences').onclick = () => closeDialog($('preferences-dialog'));
$('preferences-form').onsubmit = event => {
    event.preventDefault();
    const value = {...current.preferences};
    for (const f of document.querySelectorAll('[data-pref]'))
        value[f.dataset.pref] = f.type === 'checkbox'                    ? f.checked
                                : f.hasAttribute('data-numeric')       ? Number(f.value)
                                : [ 'number', 'range' ].includes(f.type) ? f.valueAsNumber
                                                                         : f.value;
    value.network = [...document.querySelectorAll('[data-network]')]
        .reduce((mask,input) => mask | (input.checked ? Number(input.dataset.network) : 0),0);
    if (!value.network) value.internet = false;
    send({command : 'preferences', value});
};
$('network-master').addEventListener('change',() => {
    const inputs = [...document.querySelectorAll('[data-network]')];
    if ($('network-master').checked && !inputs.some(input => input.checked)) inputs.forEach(input => input.checked = true);
});
$('network-expand').onclick = () => {
    const control = $('network-expand'), open = control.getAttribute('aria-expanded') !== 'true';
    control.setAttribute('aria-expanded',String(open));
    const content=$('network-permissions');
    revealStack(content,open,{scope:content.closest('.preferences-section'),surface:content.closest('.network-settings')});
};
for (const b of document.querySelectorAll('[data-history-view]'))
    b.onclick = () => {
        window.resetConversationWindow?.();
        send({command : 'filter', value : Number(b.dataset.historyView)});
    };
function classifyChat(value) {
    endEdit(true);
    if (!current?.archived && !current?.deleted) flushDraft();
    closeThinkingPopover();
    send({command:'classify',value});
}
$('archive-chat').onclick = () => classifyChat(1);
$('trash-chat').onclick = event => {
    event.preventDefault();
    hideTip();
    if (current.deleted) {
        chatAction({id : current.selected, title : current.title}, 'delete');
        return;
    }
    const menu = $('chat-removal');
    if (surfaceIsOpen(menu)) { surfaceClose(menu); return; }
    surfaceOpen(menu);
    positionSurface(menu, $('trash-chat'), true);
};
$('chat-removal').addEventListener('toggle', () =>
    $('trash-chat').setAttribute('aria-expanded', String(surfaceIsOpen($('chat-removal')))));
$('move-trash').onclick = () => {
    surfaceClose($('chat-removal'));
    classifyChat(2);
};
$('delete-chat').onclick = () => { surfaceClose($('chat-removal')); chatAction({id : current.selected, title : current.title}, 'delete'); };
$('restore-chat').onclick = () => classifyChat(0);
$('restore-readonly').onclick = () => classifyChat(0);
$('restore-archive').onclick = () => {
    if (waiting || current?.maintenance || current?.storageFailed || !current?.archiveTotal) return;
    endEdit(true);
    closeThinkingPopover();
    send({command:'restoreArchive'});
};
$('empty-trash').onclick = () => {
    if (waiting || current?.maintenance || current?.storageFailed || !current?.trashTotal) return;
    const body = make('p', tr('Tutte le chat nel cestino saranno eliminate definitivamente.'));
    const confirm = button('Svuota cestino', () => {
        closeDialog($('action-dialog'));
        endEdit(true);
        closeThinkingPopover();
        send({command:'emptyTrash'});
    });
    confirm.className = 'danger';
    actionDialog('Svuotare il cestino?', body,
        [button('Annulla', () => closeDialog($('action-dialog'))), confirm]);
};
for (const command of ['backup', 'export', 'restore'])
    $(command).onclick = () => {
        setFeedback($('file-result'));
        send({command});
    };
window.overtoneFileResult = (ok, restore) => {
    setFeedback($('file-result'),
        ok ? (restore ? 'Backup pronto. Chiudi e riapri OverTone per applicarlo.' : 'Operazione completata.')
           : 'Operazione non riuscita.', ok ? 'success' : 'error');
};
$('improve').onclick = () => {
    if ($('suggestion-panel').surfaceVisible) {hideSuggestion();controls();return;}
    if (improvementRequest?.result && !improvementRequest.dismissed &&
        improvementRequest.chat === current.selected && improvementRequest.source === $('prompt').value) {
        showSuggestion();return;
    }
    if (improvementRequest?.pending) return;
    const source = $('prompt').value;
    improvementRequest = {chat:current.selected,source,head:current.head,model:current.connection.model,
                          endpoint:current.connection.endpoint,pending:true,after:current.suggestionId || 0,
                          error:current.errorRevision,dismissed:false,result:''};
    $('original-prompt').replaceChildren(textSpan(source));
    hideSuggestion();
    controls();
    send({command:'improve',text:source});
};
$('discard-suggestion').onclick = () => {
    hideSuggestion();controls();$('prompt').focus({preventScroll:true});
};
$('suggestion').oninput = controls;
function applySuggestion() {
    if (!improvementRequest || improvementRequest.dismissed || improvementRequest.pending ||
        improvementRequest.chat !== current.selected || improvementRequest.source !== $('prompt').value) return false;
    $('prompt').value = $('suggestion').value;
    draftRevision++;
    improvementRequest = null;
    hideSuggestion();controls();
    $('prompt').focus({preventScroll:true});flushDraft();
    return true;
}
$('accept-suggestion').onclick = applySuggestion;
const gib = value => value ? (value / 1073741824).toLocaleString(formatLocale, {
    minimumFractionDigits : 1, maximumFractionDigits : 1
}) + ' GiB' : '—';
window.overtoneResources = r => {
    window.lastResources = r;
    if (settingsSection === 'about' && $('preferences-dialog').open) window.updateAboutResources?.();
    window.updateModelMemory?.();
    if (detailsOpen) window.renderDetails?.();
};

window.overtoneDraft = () =>
    ({chat : current?.selected || 0, text : editing ? draftBeforeEdit : $('prompt').value});
