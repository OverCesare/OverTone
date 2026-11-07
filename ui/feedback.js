'use strict';
// One presentation and lifecycle for application feedback, separate from model text.
function feedbackKind(code) {
    if (['cancelled','interrupted'].includes(code)) return 'interrupted';
    if (['busy','model_unavailable','chat_read_only','improvement_disabled'].includes(code)) return 'info';
    if (['output_limit','repetition','stream_limit','content_filter','tool_calls','context_limit',
         'invalid_input','invalid_turn','replacement_cleanup','inventory_limited','library_limited'].includes(code)) return 'warning';
    return 'error';
}
function setFeedback(host, message = '', kind = 'error') {
    if (!host) return;
    if (host.feedbackFatal && current?.storageFailed && fatalFeedbackMessage && message !== fatalFeedbackMessage)
        return;
    host.feedbackRevision = null;
    host.feedbackMessage = message;
    host.feedbackKind = kind;
    if (!message) {
        host.feedbackFatal = false;
        host.feedbackChat = null;
    }
    const response = host.classList.contains('response-status'),
          branded = response || host.id === 'notice',
          dismissible = host.id === 'notice' && !host.feedbackFatal && !current?.storageFailed,
          text = message ? tr(message) : '', signature = JSON.stringify([text,kind,dismissible]);
    if (host.feedbackSignature === signature) return;
    host.feedbackSignature = signature;
    host.hidden = !text;
    host.classList.add('app-feedback');
    if (!text) { host.replaceChildren(); return; }
    host.dataset.kind = kind;
    host.setAttribute('role', kind === 'error' && !response ? 'alert' : 'status');
    host.setAttribute('aria-atomic','true');
    host.setAttribute('aria-live',kind === 'error' && !response ? 'assertive' : 'polite');
    const content = make('span', '', 'feedback-content');
    if (branded) content.append(make('small','OverTone'));
    content.append(make('span',text,'selectable-text'));
    const glyph = {info:'info',success:'circle-check',interrupted:'circle-minus'}[kind] || 'triangle-alert';
    host.replaceChildren(icon(glyph),content);
    if (dismissible) host.append(actionButton('Chiudi avviso','x',() =>
        window.transcriptView.reflow(() => setFeedback(host))));
}
function feedbackTarget(scope = '') {
    const dialog = [...document.querySelectorAll('dialog[open]:not([data-closing])')].at(-1);
    if (scope) {
        const host = $(scope);
        return host && (host.id === 'notice' || host.closest('dialog') === dialog) ? host : $('notice');
    }
    const field = dialog?.querySelector('[data-feedback]');
    if (field) return field;
    if (route === 'chat' || route === 'models') return $(route === 'chat' ? 'notice' : 'model-error');
    let host = $('workspace-error');
    if (!host) {
        host = make('p');
        host.id = 'workspace-error';
        document.querySelector('.prepared-content>.page-heading').after(host);
    }
    return host;
}
let lastFeedbackRevision = -1, fatalFeedbackMessage = '';
function refreshFatalFeedback() {
    if (fatalFeedbackMessage) window.overtoneError(fatalFeedbackMessage,true);
}
function syncCoreFeedback(state, turns, languageChanged) {
    if (state.errorRevision !== lastFeedbackRevision) {
        lastFeedbackRevision = state.errorRevision;
        for (const host of uiElements('.app-feedback')) {
            if (host?.feedbackRevision != null) setFeedback(host);
        }
        const recorded = state.errorTurn && (state.errorTurn === state.head ||
            turns.some(turn => turn.id === state.errorTurn)),
              here = !state.errorChat || state.errorChat === state.selected;
        if (state.error && here && !recorded && !['cancelled','interrupted'].includes(state.error)) {
            const host = feedbackTarget(state.errorTurn ? 'notice' : '');
            if (state.storageFailed) {
                fatalFeedbackMessage = explain(state.error);
                host.feedbackFatal = true;
            }
            setFeedback(host,explain(state.error),feedbackKind(state.error));
            host.feedbackRevision = state.errorRevision;
            host.feedbackCode = state.error;
            host.feedbackChat = state.storageFailed ? null : state.errorChat;
        }
    }
    const notice = $('notice');
    if (!notice.hidden && notice.feedbackChat != null && notice.feedbackChat !== state.selected)
        setFeedback(notice);
    if (languageChanged) for (const host of uiElements('.app-feedback')) {
        const revision = host.feedbackRevision;
        setFeedback(host,revision != null ? explain(host.feedbackCode) : host.feedbackMessage,host.feedbackKind);
        host.feedbackRevision = revision;
    }
}
