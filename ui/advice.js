'use strict';
// The durable C catalogue supplies advice; opening a card never initiates a refresh.
const adviceView = {choices:[], signature:'', notice:null, refreshing:false, awaiting:false, request:null, seen:''},
      adviceFeedback = activityFeedback($('refresh-advice'),{canRun:() => networkAllowed(1)});
function openModelAdvice() {
    adviceView.choices = (libraryState?.adviceRecommendations || []).slice(0,1);
    adviceView.awaiting = !adviceView.choices.length && !!libraryState?.suggesting;
    adviceView.signature = '';
    adviceView.refreshing = false;
    adviceView.request = null;
    adviceView.seen = '';
    openDialog($('model-advice'));
    libraryCommand('advice',{mode:'open'});
    updateModelAdvice();
}
window.openModelAdvice = openModelAdvice;
$('show-advice').onclick = openModelAdvice;
$('close-advice').onclick = () => closeDialog($('model-advice'));
$('model-advice').addEventListener('close', () => {
    if (!$('model-advice').dataset.restoring && !$('model-advice').classList.contains('dialog-behind'))
        libraryCommand('advice',{mode:'close'});
});
$('refresh-advice').onclick = () => {
    adviceView.request = libraryState?.adviceRequest || 0;
    adviceView.refreshing = true;
    adviceFeedback.start();
    libraryCommand('advice',{mode:'refresh'});
};
$('reset-advice').onclick = () => {
    const confirm = button('Cancella apprendimento',() => {
        libraryCommand('advice',{mode:'reset'});
        closeDialog($('action-dialog'));
    });
    confirm.id = 'confirm-reset-advice';
    actionDialog('Cancella apprendimento',localLabel('p','Conversazioni, modelli e preferenze rimangono disponibili.'),[
        button('Annulla',() => closeDialog($('action-dialog'))),confirm
    ]);
};
$('restore-advice').onclick = () => {
    adviceView.awaiting = true;
    adviceView.request = null;
    libraryCommand('advice',{mode:'restoreNotices'});
};
$('advice-preferences').onclick = () => {
    $('model-advice').addEventListener('close',() => {
        $('preferences').click();
        document.querySelector('[data-settings-section="models"]').click();
    },{once:true});
    closeDialog($('model-advice'),true);
};
function updateModelAdvice() {
    if (!current || !libraryState) return;
    if (afterDetailsMotion($('show-advice'),updateModelAdvice)) return;
    if (!networkAllowed(1)) adviceView.choices = [];
    adviceView.notice = current.preferences.adviceNotices ? libraryState.recommendationNotice : null;
    $('model-advice-dot').hidden = $('advice-button-dot').hidden = !adviceView.notice;
    localAccessible($('nav-models'),adviceView.notice ? 'Modelli: nuovo consiglio' : 'Modelli');
    localAccessible($('show-advice'),adviceView.notice ? 'Consigliati per te: nuovo consiglio' : 'Consigliati per te');
    if (!$('model-advice').open || $('model-advice').hasAttribute('data-closing')) return;
    // Preference acknowledgements can precede the actual catalogue request.
    // Adopt a result only after the owner has acknowledged this collection.
    const accepted = adviceView.request === null || (libraryState.adviceRequest || 0) !== adviceView.request;
    if ((adviceView.refreshing || adviceView.awaiting) && accepted && !libraryState.suggesting) {
        adviceView.choices = (libraryState.adviceRecommendations || []).slice(0,1);
        adviceView.refreshing = adviceView.awaiting = false;
    }
    const waiting = (adviceView.refreshing || adviceView.awaiting) && (!accepted || !!libraryState.suggesting),
          chosen = waiting ? null : adviceView.choices[0], host = $('advice-options'),
          signature = JSON.stringify([language,chosen]);
    if (signature !== adviceView.signature) {
        adviceView.signature = signature;
        host.replaceChildren();
        if (chosen) {
            const card = modelSummaryCard({...chosen,verifiedChat:chosen.verified}),
                  info = actionButton('Dettagli modello','info',() => modelOptionDetails(chosen));
            info.id = 'advice-model-info';
            card.querySelector('.model-summary-heading').append(info);
            card.append(metadata([
                ...(chosen.memory ? [['Memoria','≈ '+sizes(chosen.memory)]] : []),
                ...(chosen.bytes ? [[chosen.installed ? 'Dimensione' : 'Download',(chosen.approximate ? '≈ ' : '')+sizes(chosen.bytes)]] : [])
            ]));
            host.append(card);
        }
    }
    $('advice-context').textContent = tr(purposes[current.preferences.purpose]?.[0] || 'Uso generale');
    adviceFeedback.update(adviceView.refreshing,!!libraryState.suggestionError);
    $('refresh-advice').disabled = !networkAllowed(1) || !!libraryState.suggesting || adviceFeedback.active;
    const status = $('advice-status'), offline = !networkAllowed(1) && (!chosen || !chosen.installed),
          phase = waiting ? 'loading' : offline ? 'offline' : 'empty';
    status.hidden = !!chosen && !offline;
    status.classList.toggle('has-choices',!!chosen);
    status.classList.toggle('busy',waiting);
    if (status.dataset.phase !== phase) {
        status.dataset.phase = phase;
        status.replaceChildren(icon(waiting ? 'loader' : 'box'),localLabel('span',waiting
            ? 'Ricerca dei modelli adatti' : offline ? 'Cataloghi online disattivati' : 'Non ci sono nuovi suggerimenti'));
        if (offline && !waiting) {
            const activate=button('Attiva',() => {
                enableCatalogues();
                if (!adviceView.choices.length) $('refresh-advice').onclick();
            });
            activate.id='enable-advice-catalogues';activate.className='primary';status.append(activate);
        }
    }
    if (chosen?.adviceKey && adviceView.seen !== chosen.adviceKey && current.preferences.adviceSeen !== chosen.adviceKey) {
        adviceView.seen = chosen.adviceKey;
        libraryCommand('advice',{mode:'seen',key:chosen.adviceKey});
    }
    const ignored = libraryState.adviceDismissed || 0;
    $('advice-dismissed').hidden = !ignored;
    $('advice-dismissed-count').textContent = tr('Modelli esclusi dai suggerimenti: {n}').replace('{n}',countFormat.format(ignored));
    const primary = $('acquire-advice'), job = chosen && libraryState.jobs.find(j => j.name === chosen.name &&
        ['queued','preparing','downloading','verifying','registering','paused'].includes(j.state));
    primary.hidden = !chosen;
    $('refresh-advice').hidden = false;
    $('advice-preferences').disabled = false;
    primary.textContent = tr(chosen?.installed ? 'Usa modello' : !networkAllowed(2) ? 'Attiva download' : job?.state === 'paused' ? 'Riprendi' : job ? 'Download in corso' : 'Scarica');
    primary.disabled = !chosen || !chosen.fits || chosen.space === false || !!job && job.state !== 'paused' ||
        !!chosen.installed && !!(current.active || current.improving);
    primary.onclick = () => {
        if (!chosen) return;
        if (!chosen.installed && !networkAllowed(2)) {grantNetwork(2);return;}
        const origin = primary.getBoundingClientRect();
        if (chosen.installed) {libraryCommand('select',{id:chosen.installed});closeDialog($('model-advice'));return;}
        if (job?.state === 'paused') {libraryCommand('resume',{id:job.id});return;}
        const acquire = (extra = {}) => {
            libraryCommand('advice',{mode:'acquire',name:chosen.name,kind:chosen.kind,...extra},origin);
            closeDialog($('model-advice'));
        };
        if (chosen.updateOf) window.confirmDownload({...chosen,source:'ollama'},acquire);
        else if (chosen.kind === 2 && !engineAvailable(2)) requestEngine(2,acquire);
        else acquire();
    };
}
window.updateModelAdvice = updateModelAdvice;
window.adviceSnapshot = () => {
    const d = $('model-advice'), r = d.getBoundingClientRect(),
          base = {unread:!$('model-advice-dot').hidden,buttonUnread:!$('advice-button-dot').hidden,
                  indicator:(() => {const dot=$('model-advice-dot'),r=dot.getBoundingClientRect(),
                      button=$('nav-models'),glyph=button.querySelector('.icon').getBoundingClientRect(),nav=button.getBoundingClientRect();
                      return {x:r.x,y:r.y,width:r.width,height:r.height,glyphX:glyph.x,glyphY:glyph.y,
                          navX:nav.x,navY:nav.y,navWidth:nav.width,navHeight:nav.height,
                          radius:parseFloat(getComputedStyle(button).borderTopRightRadius),stroke:parseFloat(getComputedStyle(button).borderTopWidth),colour:getComputedStyle(dot).backgroundColor};})(),learning:!!current?.preferences.adviceLearning,
                  notices:!!current?.preferences.adviceNotices,samples:libraryState?.adviceSamples || 0,sourceChoices:libraryState?.adviceRecommendations?.length || 0,sourceReady:!!libraryState?.suggestionsReady};
    if (!d.open) return {open:false,...base};
    return {open:true,...base,choices:$('advice-options').children.length,choice:adviceView.choices[0]?.option || '',
        checking:adviceView.refreshing || adviceView.awaiting,request:libraryState?.adviceRequest || 0,overflow:d.scrollWidth>d.clientWidth+1,
        controls:[...d.querySelectorAll('button')].filter(b => !hiddenUIElement(b)).slice(0,24).map(b => {
            const x=b.getBoundingClientRect();return {id:b.id,width:x.width,height:x.height,
                textOverflow:b.scrollWidth>b.clientWidth+1,outside:x.left<r.left || x.right>r.right};
        })};
};
