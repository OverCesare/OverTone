'use strict';
// Draft presentation only. Native discovery verifies the exact server/model before commit.
let connectionState = null, connectionBusy = false;
const connectionFeedback = activityFeedback($('connection-test'),{canRun:() => !connectionBusy});
window.connectionSnapshot = () => {
    const d = $('settings');
    if (!d.open) return {open:false};
    const bounds = d.getBoundingClientRect();
    return {open:true,phase:connectionState?.phase || 'draft',models:connectionState?.models.length || 0,
        context:connectionState?.context || 0,secure:$('connection-https').checked,
        auth:$('connection-auth').checked,remember:$('connection-remember').checked,
        keyPresent:!!$('key').value,saveVisible:!$('save-settings').hidden,
        overflow:d.scrollWidth>d.clientWidth+1,viewportOverflow:bounds.top<0 || bounds.bottom>innerHeight,
        controls:[...d.querySelectorAll('button,input,select')].filter(e => !hiddenUIElement(e)).map(e => {
            const r=e.getBoundingClientRect(); return {id:e.id,width:r.width,height:r.height,
                outside:r.left<bounds.left || r.right>bounds.right,textOverflow:e.scrollWidth>e.clientWidth+1};})};
};
const connectionMessages = {
    connection_secure_required:'macOS richiede HTTPS per questo indirizzo.',
    internet_disabled:'Accesso a Internet non consentito per questa funzione.',
    connection_auth:'La chiave non è valida o manca il permesso di accesso.',
    connection_protocol:'Il server non espone un protocollo compatibile.',
    connection_empty:'Il server non espone modelli disponibili.',
    connection_busy:'Il server è occupato. Riprova tra poco.',
    connection_keychain:'Impossibile salvare la chiave nel Portachiavi. Puoi usarla senza ricordarla.',
    connection_failed:'Server non raggiungibile. Controlla indirizzo e connessione.',
    connection_large:'La risposta del server supera il limite consentito.',
    connection_timeout:'Il server non ha risposto in tempo. Riprova.',
    connection_secure:'Impossibile verificare il certificato HTTPS del server.',
    connection_redirect:'Usa l’indirizzo finale del server, senza reindirizzamenti.',
    model_not_chat:'Questo modello non supporta la chat.',
    invalid_input:'Controlla l’indirizzo del server.'
};
window.connectionErrorText = code => connectionMessages[code];
function connectionIsLocal(value = $('endpoint').value) {
    try { return ['localhost','127.0.0.1','[::1]','::1'].includes(new URL(value.includes('://') ? value : 'https://'+value).hostname); }
    catch { return false; }
}
function connectionInvalidate() {
    send({command:'connectionCancel'});
    connectionState = null;
    connectionBusy = false;
    connectionFeedback.reset();
    $('connection-models').hidden = true;
    $('connection-progress').hidden = true;
    $('save-settings').hidden = true;
    $('connection-test').disabled = false;
    $('key').placeholder = '';
    setFeedback($('settings-error'));
    updateConnectionFields();
}
function updateConnectionFields() {
    $('connection-auth-fields').hidden = !$('connection-auth').checked;
    const allowed=connectionIsLocal() || networkAllowed(8);
    $('connection-network').hidden = allowed || !$('endpoint').value.trim();
    $('save-settings').disabled = connectionBusy || !allowed;
    const insecure = !$('connection-https').checked && !connectionIsLocal() && !!$('endpoint').value.trim();
    setFeedback($('connection-http-warning'),insecure ? 'Connessione non cifrata' : '', 'warning');
}
window.prepareConnection = () => {
    send({command:'connectionDraft'});
    connectionState = null;
    connectionBusy = false;
    const c = current?.connection;
    $('endpoint').value = libraryState?.owned ? '' : c?.endpoint || '';
    $('protocol').value = 'auto';
    $('key').value = '';
    $('connection-https').checked = !c?.endpoint || !connectionIsLocal(c.endpoint) || c.endpoint.startsWith('https:');
    $('connection-auth').checked = c?.error === 'connection_auth' || (c?.verified ? !!c.hasKey : !!c?.remote);
    $('connection-remember').checked = true;
    connectionInvalidate();
};
$('endpoint').oninput = () => {
    if (connectionIsLocal() && !$('endpoint').value.startsWith('https:')) $('connection-https').checked = false;
    else if (!$('endpoint').value.startsWith('http:')) $('connection-https').checked = true;
    $('key').value = '';
    connectionInvalidate();
};
for (const control of [$('protocol'),$('connection-https'),$('connection-auth'),$('key')])
    control.addEventListener(control.type === 'password' ? 'input' : 'change',connectionInvalidate);
$('enable-remote-server').onclick = () => grantNetwork(8);
$('connection-test').onclick = () => {
    if (!$('endpoint').reportValidity()) return;
    connectionFeedback.start();
    $('connection-models').hidden = true;
    send({command:'connectionProbe',endpoint:$('endpoint').value.trim(),protocol:$('protocol').value,
        https:$('connection-https').checked,useKey:$('connection-auth').checked,key:$('key').value,
        model:current?.connection.model || ''});
};
$('model').onchange = () => send({command:'connectionSelect',model:$('model').value});
window.overtoneConnection = state => {
    if (!$('settings').open || $('settings').hasAttribute('data-closing')) return;
    connectionState = state;
    const busy = ['probing','saving'].includes(state.phase), progress = $('connection-progress');
    connectionBusy = busy;
    if (progress.dataset.phase !== state.phase) {
        progress.dataset.phase = state.phase;
        progress.replaceChildren(icon('loader'),localLabel('span',state.phase === 'saving' ? 'Salvataggio' : 'Verifica in corso'));
    }
    progress.hidden = !busy;
    progress.classList.toggle('preparing',busy);
    connectionFeedback.update(busy,!!state.error);
    $('connection-test').disabled = busy || connectionFeedback.active;
    setFeedback($('settings-error'),state.error ? tr(connectionMessages[state.error] || 'Collegamento non riuscito.') : '', 'error');
    if (state.error === 'connection_auth') {
        $('connection-auth').checked = true;
        updateConnectionFields();
    }
    const selector = $('model'), signature = JSON.stringify([language,state.models]);
    if (selector.dataset.options !== signature) {
        selector.dataset.options = signature;
        selector.replaceChildren(new Option(tr('Scegli un modello'),''),...state.models.map(m => new Option(m.name,m.id)));
    }
    selector.value = state.model || '';
    selector.disabled = busy;
    $('connection-models').hidden = !state.models.length;
    const ready = state.phase === 'ready' || state.error === 'connection_keychain';
    $('save-settings').hidden = !ready;
    $('save-settings').disabled = busy;
    const preview = $('connection-preview');
    preview.hidden = !ready;
    if (ready) {
        const key = JSON.stringify([language,state.server,state.model,state.context,state.protocol,state.quantization,state.residentKnown,state.resident]);
        if (preview.dataset.summary !== key) {
            preview.dataset.summary = key;
            const model = state.models.find(m => m.id === state.model);
            preview.replaceChildren(modelSummaryCard({displayName:model?.displayName || state.model,
                quantization:state.quantization || model?.quantization,
                engine:state.protocol === 'ollama' ? 'Ollama' : tr('OpenAI compatibile'),context:state.context,
                serverConnection:true,maximumContext:true,local:connectionIsLocal(state.server),ready:true}));
            enterSurface(preview);
        }
    }
    $('key').placeholder = state.credentialStored ? tr('Salvata nel Portachiavi') : '';
};
window.overtoneConnectionCommitFailed = () => {
    connectionBusy = false;
    if (connectionState) connectionState.phase = 'ready';
    $('connection-progress').hidden = true;
    $('save-settings').disabled = false;
    $('connection-test').disabled = false;
    setFeedback($('settings-error'),'Impossibile applicare la connessione in questo momento.','error');
};
$('connection-form').onsubmit = event => {
    event.preventDefault();
    if (!connectionState || connectionBusy || $('save-settings').hidden) return;
    send({command:'connectionCommit',revision:connectionState.revision,remember:$('connection-remember').checked});
};
$('settings').addEventListener('close', () => {
    // Parent restoration temporarily closes a dialog; do not cancel a live draft then.
    if ($('settings').dataset.restoring || $('settings').classList.contains('dialog-behind')) return;
    $('key').value = '';
    send({command:'connectionDismiss'});
});
