'use strict';
let downloadOrigin = null;
let linkDownloadOrigin = null;
const libraryCommand = (action, value = {}, origin = null, prepared = false) => {
    const chosen = action === 'select' ? libraryState?.models.find(m => m.id === value.id) : null,
          kind = action === 'pull' ? 2 : chosen?.kind;
    if (!prepared && kind && !engineAvailable(kind)) {
        requestEngine(kind, () => chosen
            ? libraryCommand('setup',{mode:'use',id:chosen.id,engines:kind,allowDownloads:true})
            : libraryCommand(action,value,origin,true));
        return;
    }
    if ([ 'download', 'pull', 'confirmDownload', 'installEngine' ].includes(action) ||
        action === 'setup' && ['prepare','use'].includes(value.mode) || action === 'advice' && value.mode === 'acquire')
        downloadOrigin = origin ? {rect: origin, at: performance.now(),
            name: value.name || libraryState?.downloadCandidate?.name,
            states: new Map((libraryState?.jobs || []).map(j => [j.id,j.state]))} : null;
    send({command : 'library', action, ...value});
};
let libraryState = null, modelTab = 'installed', modelSource = 'hf', catalogSource='hf';
const dismissedEngineNotices = new Set();
function engineAvailable(kind) {
    if (libraryState?.engines.some(e => e.kind === kind)) return true;
    return kind === 2 && current?.connection.verified && current.connection.ollama &&
        /^http:\/\/(127\.0\.0\.1|localhost):11434\/?$/.test(current.connection.endpoint);
}
function requestEngine(kind, continuation) {
    const body = make('div','','settings-fields');
    body.append(localLabel('p',kind === 2 ? 'Per usare questo modello serve Ollama.' : 'Per usare questo file serve un motore GGUF compatibile.'));
    const install = button(kind === 2 ? 'Installa Ollama' : 'Installa llama.cpp',() => installEngine(kind,continuation)),
          connect = button('Collega un server',() => {
              $('action-dialog').addEventListener('close',openConnection,{once:true});
              closeDialog($('action-dialog'));
          });
    install.className = 'primary';
    actionDialog('Motore necessario',body,[connect,install]);
}
function updateEngineNotice() {
    const notice = $('engine-notice'), kind = modelSource === 'ollama' ? 2 : 1;
    if (afterDetailsMotion(notice,updateEngineNotice)) return;
    notice.hidden = !['hf','ollama','url','file'].includes(modelSource) || engineAvailable(kind) ||
        dismissedEngineNotices.has(kind);
    if (notice.hidden) return;
    const signature = [kind,language,libraryState?.jobs.some(j => j.component &&
        j.name === (kind === 2 ? 'Ollama' : 'llama.cpp') && ['queued','preparing','downloading','verifying','registering'].includes(j.state))].join(':');
    if (notice.dataset.state === signature) return;
    notice.dataset.state = signature;
    const install = button(kind === 2 ? 'Installa Ollama' : 'Installa llama.cpp',() => installEngine(kind)),
          dismiss = actionButton('Nascondi avviso','x',() => {dismissedEngineNotices.add(kind);notice.hidden=true;});
    install.className = 'constructive';
    const downloading = signature.endsWith(':true');
    install.disabled = downloading;
    if (downloading) install.textContent = tr('Installazione');
    notice.replaceChildren(icon('cpu'),localLabel('span',kind === 2
        ? 'Ollama serve per scaricare e usare questi modelli.' : 'Per usare un file GGUF serve un motore compatibile.'),install,dismiss);
    for (const child of notice.children) child.classList.add('flow-anchor');
}
function activeLibraryModel() {
    const id = libraryState?.pending || libraryState?.selected || libraryState?.requested ||
        (current?.connection.model ? current.connection.localModel : 0);
    return libraryState?.models.find(model => model.id === id);
}
function verifiedModelMark() {
    const mark = make('span','','compatibility-mark');
    mark.append(icon('shield-check'));
    mark.dataset.tip = tr('Verificato OverTone');
    mark.setAttribute('role','img');
    mark.setAttribute('aria-label',tr('Verificato OverTone'));
    return mark;
}
function runtimeObservation() {
    const runtime = window.modelMemory?.runtime;
    return runtime?.id === activeLibraryModel()?.id ? runtime : null;
}
function runtimeStatus() {
    const model = activeLibraryModel(), connectionOnly = !!current?.connection.model && !current.connection.localModel && !libraryState?.requested && !model,
          state = current?.active || current?.improving ? 'generating' : connectionOnly ? 'configured' :
              libraryState?.engine === 'ready' && !current?.ready ? 'verifying' :
              libraryState?.engine === 'ready' ? runtimeObservation()?.acceleration || 'unknown' :
              libraryState?.engine || 'stopped',
          values = {
              gpu:['circle-check','Pronto · GPU'], hybrid:['circle-check','Pronto · CPU e GPU'],
              cpu:['circle-check','Pronto · CPU'], unknown:['circle-check','Pronto · esecuzione non verificata'],
              asleep:['moon','Pronto · si ricarica alla richiesta'],
              generating:['loader','Elaborazione in corso'], configured:current?.connection.error
                  ? ['triangle-alert',current.connection.error === 'internet_disabled' ? 'Accesso a Internet disattivato.' : current.connection.error === 'connection_auth' ? 'Accesso al server richiesto'
                     : current.connection.error === 'connection_missing' ? 'Modello non disponibile sul server'
                     : current.connection.error === 'connection_busy' ? 'Server occupato'
                     : 'Server non raggiungibile'] : current?.connection.verified
                  ? !current.ready ? ['triangle-alert','Server non raggiungibile'] : current.connection.residentKnown && !current.connection.resident
                      ? ['moon','Pronto · si ricarica alla richiesta'] : ['circle-check','Pronto']
                  : ['server','Collegamento da verificare'],
              loading:['loader','Caricamento del modello'], probing:['loader','Collegamento al motore'],
              verifying:['loader','Verifica del modello'], stopping:['loader','Rilascio del modello'],
              failed:['triangle-alert','Errore di caricamento del modello'],
              stopped:['circle-minus',model ? 'Modello non caricato' : 'Nessun modello']
          }, [glyph,label] = values[state] || ['circle-minus','Stato del modello non disponibile'];
    return {state: ['gpu','hybrid','cpu','unknown'].includes(state) ? 'ready' : state, glyph, label:tr(label)};
}
function runtimeDescription() { return runtimeStatus().label; }
const sizes = bytes => {
    if (!(bytes > 0))
        return '—';
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'],
          power = Math.min(units.length - 1, Math.floor(Math.log(bytes) / Math.log(1024)));
    return (bytes / 1024 ** power).toLocaleString(formatLocale, {maximumFractionDigits : power ? 2 : 0}) +
           ' ' + units[power];
};
const libraryErrors = {
    suggestion_changed : 'Scegli di nuovo il modello suggerito.',
    suggestions_unavailable : 'Catalogo non disponibile. Riprova.',
    memory_limit : 'Memoria disponibile insufficiente. Scegli un modello più piccolo.',
    package_install : 'Installazione non riuscita. Riprova.',
    model_not_chat : 'Questo modello non supporta la chat. Scegli un modello conversazionale.',
    remote_model : 'Questo modello usa un servizio online. Scegli un modello locale o configura un collegamento esterno.',
    connection_failed : 'Collegamento al servizio non riuscito. Verifica la connessione e riprova.',
    invalid_job : 'L’attività non è più disponibile per questa azione.',
    queue_full : 'La coda è piena. Attendi o annulla un download.',
    gpu_required : 'Il motore ha caricato il modello sulla CPU. Scegli un motore con supporto GPU.',
    gpu_unverified : 'Il motore non conferma l’uso della GPU. Scegli un motore che ne esponga lo stato.',
    busy: 'Attendi il completamento dell’operazione in corso.',
    storage_write : 'Salvataggio della libreria non riuscito.',
    internet_disabled : 'Accesso a Internet non consentito per questa funzione.',
    inventory_limited : 'Scansione limitata. Puoi importare direttamente gli altri file.',
    library_limited : 'Visualizzazione limitata a 1.000 modelli e attività. Il modello in uso rimane visibile.',
    already_installed : 'Il modello è già nella libreria.',
    replacement_changed : 'La copia installata è cambiata. Conferma di nuovo il download.',
    replacement_cleanup : 'Modello installato. Il vecchio file non è stato eliminato.',
    file_changed : 'File modificato. Reimportalo o scaricalo di nuovo.',
    engine_missing :
        'Il componente necessario per questo modello non è disponibile. Puoi configurarlo in Impostazioni → Modelli.',
    engine_start : 'Avvio del motore non riuscito.',
    engine_exited : 'Il motore si è chiuso.',
    engine_timeout : 'Il motore non è pronto entro il tempo previsto.',
    invalid_gguf : 'Il file non è un modello GGUF valido.',
    import_changed : 'Il file o la selezione è cambiata. Scegli di nuovo il file.',
    memory_budget :
        'Il modello supera il budget di memoria. Aumenta il budget, riduci il contesto o scegli un modello più piccolo.',
    cpu_not_suitable : 'CPU non consigliata con questi parametri. Usa la GPU.',
    cpu_not_qualified : 'CPU non verificata per questo modello e questi parametri.',
    engine_not_ready : 'Preparazione del download non riuscita. Riprova.',
    invalid_engine :
        'Eseguibile non riconosciuto. Per altri motori configura una connessione API compatibile.',
    checksum_mismatch : 'La verifica SHA-256 non corrisponde. Riprendi per ricominciare il download.',
    resume_changed : 'Il file sul server è cambiato. Riprendi per ricominciare il download.',
    download_size : 'La dimensione ricevuta non corrisponde al file atteso.',
    disk_budget : 'Spazio libero insufficiente per questo download.',
    model_in_use : 'Arresta il modello prima di rimuoverlo.',
    file_not_owned :
        'Il file è cambiato o non è eliminabile da OverTone. Puoi rimuovere il riferimento dalla lista.',
    invalid_url : 'Usa un indirizzo HTTPS senza credenziali.',
    invalid_response : 'Risposta del servizio non valida.',
    partial_changed : 'Il download parziale è cambiato.',
    pull_failed : 'Ollama non ha completato il download.',
    pull_incomplete : 'Download Ollama incompleto.',
    response_limit : 'Risposta del servizio troppo grande.',
    not_found : 'Elemento non trovato.',
    model_unavailable : 'Preparazione del modello non riuscita.',
    http_error : 'Il servizio ha rifiutato la richiesta.'
};
window.modelErrorText = code => libraryErrors[code];
let selectedLibraryModel = 0, selectedJob = 0, selectedRepository = '', repositories = [],
    selectedVariant = null;
let repositoriesHaveMore = false;
let resultLimit = 20, ollamaResultLimit = 20, variantLimit = 12, ollamaVariantLimit = 12;
function modelPage(tab) {
    const changed = modelTab !== tab;
    if (changed) closeFloatingSurfaces();
    modelTab = tab;
    for (const name of ['installed', 'search', 'activity']) {
        $('models-' + name).hidden = name !== tab;
        $('tab-' + name).setAttribute('aria-selected', String(name === tab));
        $('tab-' + name).tabIndex = name === tab ? 0 : -1;
    }
    $('model-view-title').textContent =
        tr({installed : 'Installati', search : 'Trova', activity : 'Attività'}[tab]);
    const compact = document.querySelector('.mobile-model-tabs'), update = () => {
        document.querySelectorAll('[data-model-tab]').forEach(b => {
            const selected = b.dataset.modelTab === tab;
            b.setAttribute('aria-selected', String(selected));
            b.tabIndex = selected ? 0 : -1;
        });
    };
    if (compact && window.selectionChange)
        window.selectionChange(compact, update, '[aria-selected=true]');
    else
        update();
    updateLibraryDetails();
    window.updateAppMenu?.();
    if (changed)
        enterSurface($('models-' + tab));
}
function showWorkspace(models) {
    window.navigate(models ? 'models' : 'chat')
}
$('nav-chat').onclick = () => showWorkspace(false);
$('nav-models').onclick = () => showWorkspace(true);
for (const tab of ['installed', 'search', 'activity'])
    $('tab-' + tab).onclick = () => modelPage(tab);
$('import-from-source').onclick = () => libraryCommand('pickModel');
const modelScanFeedback = activityFeedback($('discover-models')),
      engineScanFeedback = activityFeedback($('detect-engines'));
function updateDiscoveryFeedback(busy) {
    modelScanFeedback.update(busy,!!libraryState?.error);
}
$('discover-models').onclick = () => {
    modelScanFeedback.start();
    libraryCommand('discover');
};
function catalogueFilterValues(source) {
    const value=id => $(source+'-'+id)?.value || 'all',token=id => value(id)==='all' ? '' : value(id);
    return {task:token('task'),license:token('license'),order:token('order'),maxParameters:Number(token('max-parameters')) || 0};
}
let modelSearchTimer = 0;
function searchModels(source, more = false, refresh = false) {
    clearTimeout(modelSearchTimer);
    const field = $(source === 'hf' ? 'model-query' : 'ollama-query'), query = field.value.trim();
    const view=filterViews.get(source), active=view?.sources.some(select => filterSelection(select).length && select.id!==source+'-purpose');
    if (!networkAllowed(1) || query.length<2 && !refresh && !active) {
        repositories = [];
        ollamaRepositories = [];
        selectedRepository = selectedOllamaRepository = selectedOllamaTag = '';
        libraryCommand('clearSearch', {source});
        return;
    }
    if (source === 'ollama') {
        if (!more)
            ollamaResultLimit = 20;
        if (!more) ollamaVariantLimit = 12;
        if (!more) selectedOllamaRepository = query.includes(':') ? query.split(':')[0] : '';
        if (!more) {
            selectedOllamaTag = query.includes(':') ? query : '';
            catalogVariants.ollama = selectedOllamaRepository ? null : [];
        }
        if (selectedOllamaRepository)
            ollamaRepositories = [];
        libraryCommand(selectedOllamaRepository ? 'ollamaVariants' : 'ollamaSearch',
                       selectedOllamaRepository ? {repo : selectedOllamaRepository, refresh} : {query,refresh,...catalogueFilterValues(source)});
        return;
    }
    if (!more) {
        selectedRepository = '';
        selectedVariant = null;
        catalogVariants.hf = [];
    }
    resultLimit = more ? Math.min(100, resultLimit + 20) : 20;
    if (!more) variantLimit = 12;
    libraryCommand('search',{query,limit:resultLimit,refresh,...catalogueFilterValues(source)});
}
for (const [source, form, field] of [[ 'hf', 'model-search-form', 'model-query' ],
                                     [ 'ollama', 'ollama-search-form', 'ollama-query' ]]) {
    $(form).onsubmit = e => {
        e.preventDefault();
        searchModels(source, false, true);
    };
    $(field).addEventListener('input', () => {
        clearTimeout(modelSearchTimer);
        modelSearchTimer = setTimeout(() => searchModels(source), 250);
    });
}
$('add-engine').onclick = () => libraryCommand('pickEngine', {kind : 0});
$('detect-engines').onclick = () => {engineScanFeedback.start();libraryCommand('detectEngines');};
$('reset-assistant-style').onclick = () =>
    send({command : 'preferences', value : {...current.preferences, system : null}});
function setCatalogueAccess(internet, scopes=7) {if (internet) grantNetwork(scopes); else send({command:'preferences',value:{...current.preferences,internet:false}});}
function enableCatalogues() {grantNetwork(1);}
$('enable-catalogues').onclick = () => grantNetwork(Number($('catalog-policy').dataset.needed));
window.updateCatalogPolicy = () => {
    updateEngineNotice();
    const online = networkAllowed(1), downloads = networkAllowed(2);
    const required=modelSource === 'url' ? 2 : 3, missing=required & (current?.preferences.internet ? ~current.preferences.network : 15),
          policy=$('catalog-policy');
    policy.hidden = !missing || modelSource === 'file';policy.dataset.needed=String(missing);
    policy.querySelector('span').textContent=tr(missing === 3 ? 'Cataloghi e download online disattivati.' :
        missing === 2 ? 'Download modelli disattivati' : 'Cataloghi online disattivati');
    for (const id of ['model-query', 'ollama-query'])
        $(id).disabled = !online;
    $('model-search-button').disabled = $('ollama-search-button').disabled =
        !online || !!libraryState?.searching;
    $('download-form').querySelector('button[type=submit],button:not([type])').disabled = !downloads;
    for (const id of ['download-url','download-checksum']) $(id).disabled = !downloads;
    for (const control of document.querySelectorAll('[data-network-action]'))
        control.disabled = !networkAllowed(Number(control.dataset.networkAction) || 1) || control.dataset.unavailable === 'true';
};
const engineRows = new Map();
function renderEngines() {
    const list = $('engine-list'), present = new Set();
    list.querySelector('.muted')?.remove();
    const host = $('engine-installations');
    for (const packageInfo of libraryState?.enginePackages || []) {
        let row = host.querySelector('[data-engine="'+packageInfo.kind+'"]');
        const found = libraryState.engines.some(e => e.kind === packageInfo.kind);
        if (found) { row?.remove(); continue; }
        if (!row) {
            row = make('div','','engine-install'); row.dataset.engine = packageInfo.kind;
            const title = make('span',packageInfo.name), install = button('Installa',() => installEngine(packageInfo.kind));
            title.prepend(icon('cpu')); install.id = packageInfo.kind === 2 ? 'install-engine' : 'install-llama';
            install.className = 'constructive'; row.append(title,install); host.append(row);
        }
        const job = libraryState.jobs.find(j => j.component && j.name === packageInfo.name &&
            ['queued','preparing','downloading','verifying','registering','paused'].includes(j.state));
        row.querySelector('button').disabled = !!job;
        row.querySelector('button').textContent = tr(job ? job.state === 'paused' ? 'In pausa' : 'Installazione' : 'Installa');
    }
    for (const engine of libraryState?.engines || []) {
        present.add(engine.kind);
        let controls = engineRows.get(engine.kind);
        if (!controls) {
            const row = make('div', '', 'detected-engine'), title = make('span'),
                  label = document.createTextNode(''), state = make('small'),
                  change = actionButton('Cambia eseguibile', 'folder-open',
                      () => libraryCommand('pickEngine', {kind : engine.kind})),
                  verify = actionButton('Verifica eseguibile', 'shield-check',
                      () => libraryCommand('validateEngine', {kind : engine.kind})),
                  reset = actionButton('Usa rilevamento automatico', 'rotate-ccw',
                      () => libraryCommand('resetEngine', {kind : engine.kind})),
                  licenses = actionButton('Licenze dei componenti', 'file-text',
                      () => libraryCommand('engineNotices', {kind : engine.kind}));
            verify.id = 'verify-engine-'+engine.kind;
            reset.id = 'detect-engine-'+engine.kind;
            licenses.id = 'licenses-engine-'+engine.kind;
            title.append(icon('cpu'), label);
            row.append(title, state, change, verify, reset, licenses);
            reset.classList.add('engine-discovery');
            controls = {row, label, state, change, verify, reset, licenses,
                verifyFeedback:activityFeedback(verify,{glyph:'shield-check'}),
                detectFeedback:activityFeedback(reset)};
            engineRows.set(engine.kind, controls);
            list.append(row);
        }
        controls.label.textContent = engine.name;
        for (const [control, text] of [[controls.change, 'Cambia eseguibile'],
                                      [controls.verify, 'Verifica eseguibile'],
                                      [controls.reset, engine.manual ? 'Usa rilevamento automatico' : tr('Rileva {engine}').replace('{engine}',engine.name)],
                                      [controls.licenses, 'Licenze dei componenti']]) {
            control.setAttribute('aria-label', tr(text));
            control.dataset.tip = tr(text);
        }
        controls.state.textContent = tr(engine.validation === 2 ? 'Non valido' : engine.managed ? 'Gestito da OverTone' :
                  engine.manual ? engine.validation === 1 ? 'Valido' : 'Da verificare' : 'Rilevato');
        controls.row.dataset.tip = engine.path;
        controls.verifyFeedback.update(engine.validation === 3,engine.validation === 2);
        controls.detectFeedback.update(false,!!libraryState.error);
        controls.verify.setAttribute('aria-disabled', String(engine.validation === 3));
        controls.verify.onclick = () => {
            if (controls.verify.getAttribute('aria-disabled') !== 'true')
                {controls.verifyFeedback.start();libraryCommand('validateEngine', {kind : engine.kind});}
        };
        controls.reset.onclick = () => {
            controls.detectFeedback.start();
            libraryCommand(engine.manual ? 'resetEngine' : 'detectEngines',{kind:engine.kind});
        };
        controls.licenses.hidden = !engine.managed;
    }
    for (const [kind, controls] of engineRows) {
        if (!present.has(kind)) {
            controls.verifyFeedback.dispose(); controls.detectFeedback.dispose();
            controls.row.remove();
            engineRows.delete(kind);
        }
    }
    if (!list.children.length)
        list.append(make('p', tr('Nessun motore locale rilevato.'), 'muted'));
}
$('download-form').onsubmit = e => {
    e.preventDefault();
    linkDownloadOrigin = (e.submitter || $('download-url')).getBoundingClientRect();
    const url = $('download-url').value.trim(), checksum = $('download-checksum').value.trim();
    let name = 'modello.gguf';
    try {
        name = decodeURIComponent(new URL(url).pathname.split('/').pop() || name).slice(0, 256);
    } catch {
        return;
    }
    libraryCommand('previewDownload', {url, name, checksum});
};
let pendingImportToken = 0, shownDownloadToken = 0;
function renderImport() {
    const out = $('import-preview'), item = libraryState?.importCandidate;
    out.replaceChildren();
    $('import-from-source').disabled = !!libraryState?.previewing;
    $('import-from-source').setAttribute('aria-busy', String(!!libraryState?.previewing));
    if (libraryState?.previewing) {
        out.append(make('p', tr('Verifica del file'), 'muted'));
        return;
    }
    if (!item)
        return;
    out.append(metadata([
        [ 'File', item.file ], [ 'Modello', item.name ], [ 'Formato', 'GGUF' ],
        [ 'Dimensione', sizes(item.bytes) ], [ 'Famiglia', item.family || '—' ],
        [ 'Contesto massimo', item.context ? item.context.toLocaleString(formatLocale) + ' token' : '—' ]
    ]));
    const actions = make('div', '', 'row'), cancel = button('Annulla', () => libraryCommand('cancelPreview')),
          confirm = button('Aggiungi a OverTone', () => {
              pendingImportToken = item.token;
              confirm.disabled = true;
              libraryCommand('confirmImport', {token : item.token});
          });
    confirm.className = 'primary';
    actions.append(cancel, confirm);
    out.append(actions);
}
function libraryRow(name, detail, select, selected = false, precision = '') {
    const card = document.createElement('article');
    card.className = 'library-row flow-surface' + (selected ? ' selected' : '');
    const b = button(name, select, true);
    b.className = 'row-primary';
    const title = document.createElement('h3');
    b.dataset.tip = name;
    b.setAttribute('data-tip-overflow','');
    const suffix = precision && name.endsWith(' · '+precision) ? ' · '+precision : '';
    title.append(make('span', suffix ? name.slice(0,-suffix.length) : name, 'library-name'));
    if (suffix) title.append(make('span',suffix,'library-precision'));
    const info = document.createElement('small');
    info.textContent = detail;
    b.replaceChildren(title, info);
    const actions = document.createElement('div');
    actions.className = 'row-actions flow-anchor';
    card.append(b, actions);
    card.onclick = e => {
        if (!e.target.closest('button'))
            b.click()
    };
    return {card, actions, primary : b};
}
const modelPurposes=[['Uso generale','messages-square'],['Programmazione','code'],['Studio','book-open'],
    ['Scrittura e traduzione','pencil'],['Contabilità','calculator'],['Diritto','scale']];
const modelGBChoices=[0,2,4,8,12,16,24,32,48,64,96,128,256,512];
const sizeValues=[['all','Nessun limite'],...modelGBChoices.slice(1).map(gb => [String(gb),gb+' GB']),['unknown','Non indicata']];
const contextValues=[['all','Qualsiasi'],...[8,16,32,64,128,256].map(k => [String(k*1024),k+'k token']),['unknown','Non indicato']];
const precisionValues=[...['q1','q2','q3','q4','q5','q6','q8','iq1','iq2','iq3','iq4','tq1','tq2','mxfp4','nvfp4','f16','bf16','f32']
    .map(value => [value,value.toUpperCase()]),['other','Altre'],['unknown','Non indicata']];
const licenseValues=[['all','Qualsiasi'],['apache-2.0','Apache-2.0'],['mit','MIT'],['bsd-3-clause','BSD-3-Clause'],['cc-by-4.0','CC-BY-4.0'],['other','Altre'],['unknown','Non indicata']];
const sizeFacet={id:'size',bound:'max',label:'Dimensione massima',icon:'hard-drive',values:sizeValues},
      contextFacet={id:'context',bound:'min',label:'Contesto minimo',icon:'file-text',values:contextValues},
      precisionFacet={id:'quant',label:'Quantizzazione',icon:'sliders-horizontal',multiple:true,values:precisionValues},
      nameOrder={id:'order',label:'Ordina per',icon:'list-filter',order:true,values:[['all','Nome'],['small','Più piccoli'],['large','Più grandi'],['recent','Aggiunti di recente']]},
      purposeFacet={id:'purpose',label:'Priorità d’uso',icon:'workflow',order:true,values:[['all','Nessuna preferenza'],...modelPurposes.map(([label],index) => [String(index),label])]};
const installedDefinitions=[
    {id:'state',label:'Disponibilità',icon:'circle-check',values:[['all','Tutti'],['available','Disponibili'],['missing','Non disponibili'],['selected','Scelto per la chat']]},
    {id:'library',label:'Libreria',icon:'box',values:[['all','Tutte le librerie'],['1','File GGUF'],['2','Ollama']]},
    sizeFacet,contextFacet,precisionFacet,
    {id:'features',label:'Caratteristiche',icon:'brain',multiple:true,values:[['thinking','Con ragionamento'],['vision','Immagini'],['tools','Tool'],['verified','Verificati OverTone'],['warnings','Con avvisi']]},
    {id:'license',label:'Licenza',icon:'scale',values:licenseValues},nameOrder];
const installedRows=new Map(), installedFilterIds=installedDefinitions.map(definition => 'installed-'+definition.id);
let installedFilterView=null,installedLimit=40;
function folded(text) {return String(text || '').normalize('NFKD').replace(/\p{M}/gu,'').toLocaleLowerCase(language);}
function hasModelFeature(model,value) {
    const metadata=model.metadata || model;
    return value==='thinking' ? model.reasoning===true || metadata.reasoning===true || metadata.thinkingKnown && metadata.thinking?.some(mode => !['off','none'].includes(mode)) :
        value==='verified' ? !!model.verifiedChat : value==='warnings' ? modelWarnings(model).length>0 :
        value==='vision' ? model.vision===true || metadata.vision===true : value==='tools' ? model.tools===true || metadata.tools===true : false;
}
function modelFilterFacts(model,prefix,selectedId) {
    const installed=!!model.installed,license=String(model.license || model.metadata?.license || '').toLowerCase(),
          known=typeof model.fitsMemory==='boolean' && (installed || typeof model.fitsDisk==='boolean');
    return {library:String(model.kind),size:model.bytes || 0,context:model.context || 0,quant:model.precision || 'unknown',
        license:!license ? 'unknown' : licenseValues.some(([value]) => value===license) ? license : 'other',
        features:['thinking','vision','tools','verified','warnings'].filter(value => hasModelFeature(model,value)),
        selected:model.id===selectedId,available:!!model.available && !model.metadata?.fileChanged,installed,acquiring:!!model.job,
        resources:known ? model.fitsMemory && (installed || model.fitsDisk) ? 'fits' : 'limited' : 'unknown',
        execution:model.cloud ? 'cloud' : 'local',scope:prefix==='installed' ? 'installed' : 'variant'};
}
function modelFacetTest(facts,key,value) {
    return value==='all' || (key==='size' ? value==='unknown' ? !facts.size : facts.size>0 && facts.size<=Number(value)*1e9 :
        key==='context' ? value==='unknown' ? !facts.context : facts.context>=Number(value) :
        key==='features' ? facts.features.includes(value) : key==='state' ? facts.scope==='installed' ?
            value==='selected' ? facts.selected : value==='available' ? facts.available : !facts.available :
            value==='installed' ? facts.installed : value==='acquiring' ? facts.acquiring : !facts.installed : facts[key]===value);
}
function filterModels(prefix,models) {
    const started=performance.now(),view=filterViews.get(prefix);if (!view) return models;
    const selectedId=activeLibraryModel()?.id || 0,
          definitions=prefix==='installed' ? installedDefinitions : variantDefinitions,
          facets=view.sources.filter(source => !source.hasAttribute('data-filter-order')).map(source => ({source,
              key:source.id.slice(prefix.length+1),selected:filterSelection(source),
              definition:definitions.find(value => source.id===prefix+'-'+value.id)})),
          counts=facets.map(facet => new Map(facet.definition.values.map(([value]) => [value,0]))),matched=[];
    for (const model of models) {
        const facts=modelFilterFacts(model,prefix,selectedId), failed=facets.reduce((mask,facet,index) => mask | (facet.selected.length &&
            !(facet.key==='features' ? facet.selected.every(value => modelFacetTest(facts,facet.key,value)) :
                facet.selected.some(value => modelFacetTest(facts,facet.key,value))) ? 1<<index : 0),0);
        if (!failed) matched.push(model);
        facets.forEach((facet,index) => {
            if (failed & ~(1<<index)) return;
            for (const [value] of facet.definition.values) if (modelFacetTest(facts,facet.key,value) &&
                (facet.key!=='features' || facet.selected.every(chosen => modelFacetTest(facts,facet.key,chosen))))
                counts[index].set(value,counts[index].get(value)+1);
        });
    }
    facets.forEach((facet,index) => filterOptions(facet.source.id,facet.definition.values,value => counts[index].get(value)));
    view.summary(matched.length,models.length);view.workMs=performance.now()-started;return matched;
}
function sortModelRows(models,order,purpose='all') {
    const collator=new Intl.Collator(formatLocale,{numeric:true,sensitivity:'base'}), relevance=model => purpose==='all' ? 0 : Number(!!(model.purposes & 1<<Number(purpose)));
    return models.sort((a,b) => relevance(b)-relevance(a) || (order==='source' ? 0 : order==='recent' ? b.id-a.id :
        (order==='small' || order==='large') && a.bytes!==b.bytes ? !(a.bytes>0) ? 1 : !(b.bytes>0) ? -1 :
        (order==='small' ? a.bytes-b.bytes : b.bytes-a.bytes) : collator.compare(modelLabel(a),modelLabel(b)) || (a.id || 0)-(b.id || 0)));
}
function renderInstalled() {
    if (afterDetailsMotion($('model-list'),renderInstalled) || !libraryState) return;
    const query=folded($('installed-query').value.trim()),
          models=libraryState.models.filter(model => !query || folded([model.name,modelLabel(model),model.metadata?.family,
              model.metadata?.license,model.quantization,model.file,model.kind===1 ? 'GGUF' : 'Ollama'].join(' ')).includes(query));
    const matches=sortModelRows(filterModels('installed',models),$('installed-order').value),list=matches.slice(0,installedLimit);
    let more=$('installed-more');
    if (matches.length>installedLimit) {
        if (!more) {more=button('Carica altri modelli',() => {installedLimit+=40;renderInstalled();});more.id='installed-more';more.className='catalog-more';}
    } else {more?.remove();more=null;}
    const out = $('model-list'), present = new Set(list.map(m => m.id));
    out.querySelector('.model-empty')?.remove();
    for (const [id, row] of installedRows) {
        if (!present.has(id)) {
            row.card.remove();
            installedRows.delete(id);
        }
    }
    for (const [index, m] of list.entries()) {
        const engine = m.kind === 1 ? 'llama.cpp' : 'Ollama', provenance = m.kind === 2 ? 'Ollama'
                                                                           : m.managed
                                                                               ? 'OverTone'
                                                                               : tr('File o cache esterna');
        let row = installedRows.get(m.id);
        if (!row) {
            row = libraryRow(modelLabel(m), '', () => {
                selectedLibraryModel = m.id;
                renderInstalled();
                updateLibraryDetails(true)
            });
            row.title = row.primary.querySelector('h3');
            row.label = row.title.firstChild;
            row.badge = make('span', tr('Non disponibile'), 'badge');
            row.title.append(row.badge);
            row.info = row.primary.querySelector('small');
            row.warning = actionButton('Mostra avvisi nei dettagli', 'triangle-alert', () => {
                selectedLibraryModel = m.id;
                updateLibraryDetails(true);
            });
            row.remove = actionButton('', 'trash', () => confirmModelRemoval(row.model), 'danger');
            row.actions.append(row.warning, row.remove);
            installedRows.set(m.id, row);
        }
        row.model = m;row.card.dataset.model=String(m.id);
        row.primary.dataset.tip = row.label.textContent = modelLabel(m);
        row.badge.hidden = !!m.available && !m.metadata?.fileChanged;
        row.badge.textContent = tr(m.metadata?.fileChanged ? 'Da verificare' : 'Non disponibile');
        row.verified ||= verifiedModelMark();
        row.verified.dataset.tip = tr('Verificato OverTone');
        row.verified.setAttribute('role','img');
        row.verified.setAttribute('aria-label',tr('Verificato OverTone'));
        row.verified.hidden = !m.verifiedChat;
        row.title.append(row.verified);
        row.info.textContent =
            [ engine, sizes(m.bytes), provenance === engine ? '' : provenance ].filter(Boolean).join(' · ');
        row.card.classList.toggle('selected', m.id === selectedLibraryModel);
        row.primary.setAttribute('aria-label', tr('Mostra dettagli di') + ' ' + modelLabel(m));
        row.warning.hidden = !modelWarnings(m).length;
        row.remove.disabled = libraryState.busy;
        const removing = libraryState.removing === m.id;
        if (row.removing !== removing) {
            row.removing = removing;
            row.remove.replaceChildren(icon(removing ? 'rotate-ccw' : 'trash'));
            row.remove.setAttribute('aria-busy', String(removing));
        }
        row.remove.hidden = !!m.hidden;
        row.remove.setAttribute('aria-label', tr('Rimuovi') + ' ' + modelLabel(m));
        row.remove.dataset.tip = tr(removing ? 'Disinstallazione' : 'Rimuovi') + ' ' + modelLabel(m);
        if (out.children[index] !== row.card)
            out.insertBefore(row.card, out.children[index] || null);
    }
    if (more) out.append(more);
    if (!list.length) {
        const empty=emptyModel(tr(libraryState.models.length ? 'Nessun risultato con questi filtri.' : 'Nessun modello locale trovato.'));
        if (installedFilterView?.sources.some(source => !source.hasAttribute('data-filter-order') && filterSelection(source).length)) empty.append(button('Azzera filtri',() => installedFilterView.clear()));
        out.append(empty);
    }
}
function emptyModel(text) {
    const p = make('div','','model-empty');
    p.dataset.label = text;
    p.append(icon('box'),make('p',text));
    return p;
}
function modelSummaryCard(model) {
    const card = make('section','','model-summary-card'), header = make('div','','model-summary-heading');
    header.append(icon('box'),make('h3',modelLabel(model)));
    if (model.verifiedChat) header.append(verifiedModelMark());
    card.append(header,metadata([
        [model.serverConnection ? 'Protocollo' : 'Motore',model.engine || (model.kind === 1 ? 'llama.cpp' : 'Ollama')],
        [model.serverConnection ? 'Server' : 'Posizione',model.local === undefined ? undefined : tr(model.local === false ? 'Server remoto' : 'Questo computer')],
        ...(model.context ? [[model.maximumContext ? 'Contesto massimo' : 'Contesto',countFormat.format(model.context)+' token']] : []),
        ['Stato',model.ready === undefined ? undefined : model.phase || tr(model.ready ? 'Pronto' : 'Disponibile')]
    ].filter(row => row[1] !== undefined)));
    return card;
}
// Keep one selected family's variants per catalogue, independent of the next family page.
// null means a family has been requested but its catalogue has not answered yet.
const catalogVariants = {hf:[], ollama:[]};
let selectedOllamaRepository = '', selectedOllamaTag = '', ollamaRepositories = [];
function catalogRows(out, entries, build, more, loading, showEmpty = true) {
    const old = new Map([...out.children].filter(n => n.dataset.key).map(n => [n.dataset.key,n]));
    const retained = new Set(), active = document.activeElement,
          focusedCard = out.contains(active) ? active.closest('[data-key]') : null,
          focusedIndex = focusedCard ? [...focusedCard.querySelectorAll('button')].indexOf(active) : -1,
          oldMore = out.querySelector('.catalog-more');
    if (more && oldMore) {
        oldMore.onclick = more.onclick;
        oldMore.disabled = more.disabled;
        oldMore.textContent = more.textContent;
        more = oldMore;
    }
    if (more) { more.classList.add('catalog-more'); retained.add(more); }
    let cursor = out.firstChild;
    for (const entry of entries) {
        const {selected,...data} = entry, key = entry.name,
              signature = JSON.stringify([language,formatLocale,data]), prior = old.get(key);
        const node = prior?.catalogSignature === signature ? prior : build(entry);
        if (node !== prior || node.classList.contains('selected') !== selected) {
            node.classList.toggle('selected', selected);
            const primary = node.querySelector('.row-primary');
            primary.setAttribute(primary.hasAttribute('aria-controls') ? 'aria-expanded' : 'aria-pressed',String(selected));
        }
        node.dataset.key = key;
        node.catalogSignature = signature;
        retained.add(node);
        if (node !== cursor) out.insertBefore(node, cursor);
        cursor = node.nextSibling;
    }
    let empty = null;
    if (!entries.length && showEmpty) {
        const label = tr(loading ? 'Ricerca in corso' : 'Nessun risultato con questi filtri.'),
              previous = out.querySelector('.model-empty');
        empty = previous?.dataset.label === label &&
                previous.classList.contains('catalog-loading') === !!loading ? previous : emptyModel(label);
        if (loading && !empty.classList.contains('catalog-loading')) {
            empty.classList.add('catalog-loading');
            empty.querySelector('use')?.setAttribute('href','#i-loader');
        }
        retained.add(empty);
    }
    for (const node of [...out.children])
        if (!retained.has(node)) node.remove();
    if (more && out.lastChild !== more) out.append(more);
    if (focusedCard && !active.isConnected) {
        const replacement = [...out.children].find(n => n.dataset.key === focusedCard.dataset.key);
        replacement?.querySelectorAll('button')[Math.max(0,focusedIndex)]?.focus({preventScroll:true});
    }
    if (empty && out.lastChild !== empty) out.append(empty);
    out.setAttribute('aria-busy', String(loading));
}
function setVariantWaiting(panel, waiting) {
    const status = panel.querySelector('.variant-progress .sr-only'), label = waiting ? tr('Caricamento') : '';
    if (status.textContent !== label) status.textContent = label;
    if (panel.hasAttribute('data-waiting') === waiting) return;
    panel.toggleAttribute('data-waiting',waiting);
    clearTimeout(panel.progressTimer);
    if (waiting)
        panel.progressTimer = setTimeout(() => panel.setAttribute('data-show-progress',''),motionTiming('fast').duration);
    else panel.removeAttribute('data-show-progress');
}
// Commit the grid once. Move the panel, the disclosure controls and the painted
// edges of visible cards together; text and corner radii never scale.
function catalogLayout(panel, repository) {
    const grid = panel.parentElement, families = grid.firstElementChild, open = !!repository,
          previous = grid.variantAnchor || null, anchor = open ? families.querySelector(':scope>.selected') : null,
          running = grid.catalogMotion, shown = running ? running.open : panel.dataset.visible === 'true';
    if (shown === open && previous === anchor) return;
    if (!open) setVariantWaiting(panel,false);
    const stacked = getComputedStyle(families).display === 'contents';
    const place = () => {
        previous?.removeAttribute('data-variant-anchor');
        anchor?.setAttribute('data-variant-anchor','');
        grid.variantAnchor = anchor;
    };
    if (!stacked && (shown === open || running?.reverse)) {
        place();
        if (shown !== open) running.reverse();
        return;
    }
    const started = performance.now(), animate = motionEnabled() && !hiddenUIElement(grid),
          viewport = animate && grid.closest('.model-view-surface').getBoundingClientRect(),
          before = animate ? [...families.children].flatMap(card => {
              const rect = surfaceRect(card);
              if (rect.bottom < viewport.top-80 || rect.top > viewport.bottom+80) return [];
              const actions = card.querySelector('.row-actions');
              return [{card,rect,actions,fromAction:actions?.getBoundingClientRect()}];
          }) : [],
          panelBefore = animate && !panel.hidden ? panel.getBoundingClientRect() : null;
    grid.catalogMotion?.finish();
    place();
    panel.hidden = false;
    panel.inert = !open;
    panel.dataset.visible = String(open);
    const finishVisibility = (visible = open) => {
        panel.dataset.visible = String(visible);
        panel.hidden = panel.inert = !visible;
        if (!visible) {
            const list = panel.querySelector('.variant-list');
            setVariantWaiting(panel,false);
            list.leaving?.cancel();
            list.leaving = null;
            list.inert = false;
            delete panel.dataset.repository;
        }
    };
    if (!animate) { finishVisibility(); return; }
    const timing = {...motionTiming(open ? 'panel' : 'panel-exit'),fill:'both'},
          dest = panel.getBoundingClientRect(),
          x = panelBefore ? panelBefore.left-dest.left : stacked ? 0 : grid.getBoundingClientRect().right-dest.left,
          y = panelBefore ? panelBefore.top-dest.top : stacked ? -6 : 0,
          moves = before.map(item => ({...item,after:item.card.getBoundingClientRect(),
              toAction:item.actions?.getBoundingClientRect(),paint:surfacePaint(item.card)}));
    const effects = [], cleanups = new Map();
    const move = (node,dx,dy) => {
        if (Math.abs(dx)+Math.abs(dy)<.25) return;
        effects.push(node.animate([{transform:'translate('+dx+'px,'+dy+'px)'},{transform:'none'}],timing));
    };
    if (stacked && !open) {
        effects.push(panel.animate([{transform:`translateY(${y}px)`,opacity:1},
            {transform:`translateY(${y-6}px)`,opacity:0}],timing));
    } else {
        move(panel,x,y);
        if (stacked && !panelBefore) effects.push(panel.animate([{opacity:0},{opacity:1}],timing));
    }
    for (const {card,rect,after,actions,fromAction,toAction,paint} of moves) {
        const dy = rect.top-after.top;
        move(card,0,dy);
        if (actions) move(actions,fromAction.left-toAction.left,fromAction.top-toAction.top-dy);
        const skin = resizeSurface(card,rect,after,paint,timing);
        if (skin.effects.length) {effects.push(...skin.effects);cleanups.set(card,skin.clean);}
    }
    if (!effects.length) {finishVisibility();return;}
    let driving = effects, changes = null, settled = 0;
    const motion = grid.catalogMotion = {open,finish:() => finish()};
    function finish(newOwner) {
        if (grid.catalogMotion !== motion) return;
        grid.catalogMotion = null;
        window.removeEventListener('resize',motion.finish);
        for (const effect of driving) effect.cancel();
        if (newOwner) {
            for (const [card,clean] of cleanups) newOwner.set(card,clean);
        } else {
            const restore = [...cleanups.values()].map(clean => clean()).filter(Boolean);
            if (restore.length) {void getComputedStyle(grid).opacity;for (const reset of restore) reset();}
        }
        finishVisibility(motion.open);
    }
    const observe = (group,since) => {
        const last = group[0];
        last.finished.then(() => {
            if (group === driving) motion.finish();
        },() => {});
        measureMotion('variants',last,performance.now()-since);
    };
    let ease;
    function remaining(time) {
        ease ||= layoutEasing(timing);
        if (!changes) return 1-ease(time-effects[0].startTime);
        let reached=settled;
        for (const change of changes) reached+=change.delta*ease(time-change.at);
        return 1-Math.max(0,Math.min(1,reached));
    }
    // Hand the inspector the unfinished trajectory, including velocity. A DOM
    // rectangle describes a layout sample, not the compositor's next frame.
    if (!stacked) motion.takeFlow = newOwner => {
        if (!changes && effects[0].startTime===null) {motion.finish();return null;}
        const residual = new Map([[panel,{x,y}]]);
        for (const {card,rect,after,actions,fromAction,toAction} of moves) {
            const dy=rect.top-after.top;
            residual.set(card,{y:dy,width:rect.width-after.width});
            if (actions) residual.set(actions,{x:fromAction.left-toAction.left,y:fromAction.top-toAction.top-dy});
        }
        const target=motion.open===open ? 0 : 1, leaving=motion.open ? null : panel,
              start=effects[0].startTime, intent=changes?.at(-1).at ?? start;
        // Capture the original clock before cancel() clears its startTime.
        if (!changes) changes=[{at:start,delta:1}];
        finish(newOwner);
        if (leaving) leaving.hidden=false;
        return {residual,leaving,end:intent+timing.duration,remaining:time => remaining(time)-target};
    };
    if (!stacked) motion.reverse = () => {
        const since = performance.now();
        motion.open = !motion.open;
        panel.inert = !motion.open;
        const start = effects[0].startTime;
        if ((!changes && start === null) || !motionEnabled() || hiddenUIElement(grid)) {
            motion.finish();
            return;
        }
        changes ||= [{at:start,delta:1}];
        changes.push({at:since,delta:motion.open===open ? 1 : -1});
        // Rebuild the same trajectory on the document timeline, adding a smooth
        // change of destination. At the click both its position and velocity match.
        // Include a short history for the rendering pipeline. These are curve
        // control points; WebKit interpolates them at the display's own cadence.
        const duration = timing.duration, from = since-64, end = since+duration;
        while (changes[0].at+duration<from) settled+=changes.shift().delta;
        const points=layoutSamples(from,end,time => ({origin:remaining(time)}));
        const previous=driving;
        driving=effects.map(original => {
            const source=original.effect.getKeyframes()[0].transform, m=new DOMMatrixReadOnly(source),
                  scale=m.a!==1 || m.d!==1;
            const keys=points.map(({offset,origin})=>({offset, transform:scale ?
                `scale(${1+(m.a-1)*origin},${1+(m.d-1)*origin})` :
                `translate(${m.e*origin}px,${m.f*origin}px)`}));
            const next=original.effect.target.animate(keys,{duration:end-from,fill:'both'});
            next.startTime=from;
            return next;
        });
        for (const effect of previous) effect.cancel();
        observe(driving,since);
    };
    window.addEventListener('resize',motion.finish,{once:true});
    observe(effects,started);
}
// Keep the old title and rows together until the new reply is available.
// Its actions become inert immediately. A fast sequence paints only the latest family.
function prepareVariantList(source, panel, list, repository, loading) {
    const changed = panel.dataset.repository !== repository,
          animate = motionEnabled() && !hiddenUIElement(panel);
    let retained = !!panel.dataset.repository && !!list.querySelector('.library-row');
    setVariantWaiting(panel,loading);
    if (list.leaving) {
        const finished = list.leaving.playState === 'finished';
        if (animate && changed && !loading && !finished) return false;
        list.leaving.cancel();
        list.leaving = null;
        if (finished && !loading) retained = false;
    }
    if (loading && retained) {
        list.inert = changed;
        list.setAttribute('aria-busy','true');
        return false;
    }
    if (animate && changed && retained) {
        list.inert = true;
        list.setAttribute('aria-busy','true');
        const exit = list.animate([{opacity:getComputedStyle(list).opacity},{opacity:0}],
                                  {...motionTiming('fast'),fill:'forwards'});
        list.leaving = exit;
        exit.finished.then(() => renderModelCatalog(source),() => {});
        return false;
    }
    if (changed) list.scrollTop = 0;
    list.inert = false;
    if (animate && !loading && (changed || !retained && list.getAttribute('aria-busy') === 'true')) {
        const entrance = list.animate([{opacity:0,transform:'translateY(5px)'},{opacity:1,transform:'none'}],motionTiming());
        measureMotion('surface',entrance);
    }
    panel.dataset.repository = repository;
    return true;
}
function renderModelCatalog(source) {
    const state = libraryState;
    if (!state) return;
    const hf = source === 'hf', familiesKind = hf ? 'search' : 'ollamaSearch',
          variantsKind = hf ? 'variants' : 'ollamaVariants',
          selected = hf ? selectedRepository : selectedOllamaRepository,
          limit = hf ? resultLimit : ollamaResultLimit, variantCount = hf ? variantLimit : ollamaVariantLimit;
    if (!state.searching) {
        if (state.catalogKind === familiesKind) {
            if (hf) { repositories = state.catalog; repositoriesHaveMore = !!state.catalogHasMore; }
            else ollamaRepositories = state.catalog;
        } else if (state.catalogKind === variantsKind && state.catalogRepository === selected)
            catalogVariants[source] = state.catalog;
    }
    if (afterDetailsMotion($(hf ? 'model-results' : 'ollama-results'),() => renderModelCatalog(source))) return;
    const viewport = document.querySelector('.model-view-surface'), scrollTop = viewport.scrollTop,
          out = $(hf ? 'model-results' : 'ollama-results'),
          all = hf ? repositories : ollamaRepositories,
          families = all.slice(0,limit), panel = $(hf ? 'variant-panel' : 'ollama-variant-panel');
    if (selected && !families.some(x => x.name === selected)) families.unshift({name:selected});
    const purpose=$(source+'-purpose').value,
          familyEntries=families.map(item => ({...item,selected:item.name===selected,
              installed:state.models.some(model => model.available && !model.metadata?.fileChanged &&
                  (hf ? model.metadata?.sourceURL===item.sourceURL : model.kind===2 && model.name.split(':')[0]===item.name))}));
    if (purpose!=='all') familyEntries.sort((a,b) => Number(!!(b.purposes & 1<<Number(purpose)))-Number(!!(a.purposes & 1<<Number(purpose))));
    const loadingFamilies = state.searching && state.catalogKind === familiesKind;
    // Removing the focused footer while waiting shortens the scroll owner and clamps its offset.
    const more = ((hf ? repositoriesHaveMore : all.length >= limit && limit < 100) || loadingFamilies && out.querySelector('.catalog-more')) ? button('Carica altri risultati', () => {
        if (hf) searchModels('hf', true);
        else { ollamaResultLimit += 20; renderOllamaCatalog(); }
    }) : null;
    if (more) more.disabled = loadingFamilies;
    catalogRows(out, familyEntries, item => {
        const row = libraryRow(item.name, hf ? [item.downloads === undefined ? '' :
            quantity('downloads', item.downloads), item.license].filter(Boolean).join(' · ') ||
            'Hugging Face' : 'Ollama', () => {
            clearTimeout(modelSearchTimer);
            const collapse = item.name === (hf ? selectedRepository : selectedOllamaRepository) && !libraryState.error,
                  next = collapse ? '' : item.name;
            if (hf) { selectedRepository = next; selectedVariant = null; variantLimit = 12; }
            else { selectedOllamaRepository = next; selectedOllamaTag = ''; ollamaVariantLimit = 12; }
            catalogVariants[source] = collapse ? [] : null;
            if (!collapse) libraryCommand(variantsKind,{repo:next});
            renderModelCatalog(source);
        }, item.selected);
        if (item.installed) row.primary.querySelector('h3').append(make('span', tr('Installato'), 'badge'));
        const disclosure = make('span', '', 'library-disclosure');
        disclosure.setAttribute('aria-hidden', 'true');
        disclosure.append(icon('chevron-right'));
        row.actions.append(disclosure);
        row.primary.setAttribute('aria-controls', panel.id);
        return row.card;
    }, more, state.searching && state.catalogKind === familiesKind,
       !!selected || $(hf ? 'model-query' : 'ollama-query').value.trim().length >= 2);
    catalogLayout(panel,selected);
    const variants = $(hf ? 'variant-list' : 'ollama-variants'),
          loadingVariants = catalogVariants[source] === null && networkAllowed(1) && !state.error;
    if (selected && prepareVariantList(source,panel,variants,selected,loadingVariants)) {
        const title = $(hf ? 'variant-title' : 'ollama-variant-title');
        title.dataset.tip = title.textContent = selected;
        const models=(catalogVariants[source] || []).map(item => ({...item,
            installed:state.models.some(model => model.available && !model.metadata?.fileChanged &&
                (hf ? item.checksum && model.metadata?.checksum===item.checksum : model.kind===2 && model.name===item.name)),
            job:state.jobs.find(job => (hf ? item.checksum && job.metadata?.checksum===item.checksum : job.source==='ollama' && job.name===item.name) &&
                ['queued','preparing','downloading','verifying','registering','paused'].includes(job.state))?.state || '',
            selected:hf ? selectedVariant?.name===item.name : selectedOllamaTag===item.name,busy:state.busy,online:networkAllowed(2)}));
        const items=filterModels(source+'-variant',models),
              entries=sortModelRows(items,$(source+'-variant-order').value,purpose).slice(0,variantCount),
              moreVariants=items.length>variantCount ? button('Carica altre varianti',() => {
                  if (hf) variantLimit+=12;else ollamaVariantLimit+=12;renderModelCatalog(source);
              }) : null;
        catalogRows(variants, entries, item => {
            const acquiring = !!item.job, facts = hf ? sizes(item.bytes) :
                [item.cloud ? 'Cloud' : '',item.bytes ? (item.sizeApproximate ? '≈ ' : '') + sizes(item.bytes) : '',
                 item.context ? item.context.toLocaleString(formatLocale) + ' token' : ''].filter(Boolean).join(' · ') || 'Ollama',
                  row = libraryRow(modelLabel(item), facts, () => {
                      if (hf) selectedVariant = item; else selectedOllamaTag = item.name;
                      renderModelCatalog(source);
                      updateLibraryDetails(true);
                  }, item.selected, item.quantization || item.metadata?.quantization),
                  label = item.cloud ? 'Disponibile tramite API' : item.installed && !acquiring ? 'Scarica di nuovo' :
                      item.job === 'queued' ? 'In coda' : item.job === 'paused' ? 'In pausa' : acquiring ? 'Download in corso' : 'Scarica',
                  glyph = item.cloud ? 'cloud' : item.job === 'queued' ? 'clock-3' :
                      item.job === 'paused' ? 'pause' : acquiring ? 'loader' : 'download',
                  download = actionButton(label,glyph,() => {
                      if (hf) selectedVariant = item;
                      const origin = download.getBoundingClientRect();
                      window.confirmDownload(hf ? item : {...item,source:'ollama'}, extra =>
                          libraryCommand(hf ? 'download' : 'pull', hf ? {...item,...extra} : {name:item.name,bytes:item.bytes,...extra},origin));
                  }, 'constructive');
            if (item.installed) {
                const installed = make('span', '', 'catalog-installed');
                installed.append(icon('circle-check'));
                installed.setAttribute('role', 'img');
                installed.setAttribute('aria-label', tr('Installato'));
                installed.dataset.tip = tr('Installato');
                row.primary.querySelector('h3').append(installed);
            }
            download.disabled = item.cloud || acquiring || item.busy || (hf && !item.url);
            download.dataset.networkAction = '2';
            download.dataset.unavailable = String(download.disabled);
            download.disabled ||= !item.online;
            download.dataset.acquisition = item.job === 'queued' ? 'queued' :
                item.job === 'paused' ? 'paused' : acquiring ? 'active' : '';
            row.actions.append(download);
            return row.card;
        }, moreVariants, loadingVariants, !loadingVariants && (!state.error || models.length > 0));
    }
    // The scroll owner survives updates. Appending rows must not follow the displaced button.
    viewport.scrollTop = scrollTop;
    updateLibraryDetails();
}
function renderCatalog() { renderModelCatalog('hf'); }
function renderOllamaCatalog() { renderModelCatalog('ollama'); }
function jobLabel(j) {
    const labels = {
        queued : 'In coda',
        preparing : 'Preparazione',
        downloading : 'Download',
        verifying : 'Verifica file',
        registering : 'Registrazione modello',
        registration_failed : 'Da registrare',
        paused : 'In pausa',
        cancelled : 'Annullato',
        complete : 'Completato',
        failed : 'Non riuscito',
        pending : 'In preparazione'
    };
    return tr(j.component && j.phase === 'registering' ? 'Installazione motore' : labels[j.phase] || labels[j.state] || 'Stato non disponibile');
}
function jobError(j) {
    if (!jobHasError(j)) return '';
    return tr(j.retryRegistration ? 'File scaricato. Riprova la registrazione.' :
              libraryErrors[j.error] || explain(j.error));
}
function jobHasError(j) {
    // A resume can retain a checksum/repair marker for the worker. While queued
    // or running it is not a new failure of the accepted retry.
    return !!j.error && !['queued','preparing','downloading','verifying','registering'].includes(j.state);
}
function transferEstimate(j) {
    if (j.state !== 'downloading' || !(j.bytesPerSecond > 0))
        return '';
    const remaining = j.total > j.received ? Math.ceil((j.total - j.received) / j.bytesPerSecond) : 0;
    return sizes(j.bytesPerSecond) + '/s' +
           (remaining > 0 ? ' · ' + (remaining < 60 ? remaining + ' s' : Math.ceil(remaining / 60) + ' min')
                          : '');
}
function jobFraction(job) {
    if (job?.state === 'complete') return 1;
    if (!job?.total || job.progressKnown === false) return null;
    const ratio = Math.max(0, Math.min(1, job.received / job.total));
    return job.state === 'downloading' ? Math.min(.995, ratio) : ratio;
}
function jobProgressText(j) {
    const amount = n => n === 0 ? '0 B' : sizes(n);
    return [j.total && j.progressKnown !== false ? amount(j.received)+' / '+amount(j.total) : j.received ? amount(j.received) : '',
            transferEstimate(j)].filter(Boolean).join(' · ');
}
function jobActions(j, clearFinished = true) {
    const actions = [], phase = j.phase;
    const active = [ 'preparing', 'downloading', 'verifying', 'registering' ].includes(j.state);
    if (active || j.state === 'queued') {
        actions.push(
            actionButton('Pausa download', 'pause', () => libraryCommand('pause', {id : j.id})));
        actions.push(
            actionButton('Annulla download', 'square', () => libraryCommand('cancelJob', {id : j.id}), 'danger'));
    } else {
        if (j.state === 'paused') {
            const retry = actionButton(phase === 'registration_failed' ? 'Riprova registrazione' : 'Riprendi',
                'play', () => libraryCommand('resume', {id : j.id}));
            if (!j.retryRegistration && !j.resumeOffline)
                retry.dataset.networkAction = String(j.component ? 4 : 2);
            retry.dataset.unavailable = 'false';
            retry.disabled = !j.retryRegistration && !j.resumeOffline && !networkAllowed(j.component ? 4 : 2);
            actions.push(retry);
            actions.push(actionButton('Annulla download', 'square', () => libraryCommand('cancelJob', {id : j.id}), 'danger'));
        }
        if (j.state === 'cancelled') {
            const restart = actionButton('Scarica di nuovo', 'download', () =>
                libraryCommand('restart', {id:j.id}), 'constructive');
            restart.dataset.networkAction = String(j.component ? 4 : 2);
            restart.dataset.unavailable = 'false';
            restart.disabled = !networkAllowed(j.component ? 4 : 2);
            actions.push(restart);
        }
        if (j.error === 'replacement_cleanup')
            actions.push(actionButton('Elimina il vecchio file', 'trash', () =>
                libraryCommand('cleanupReplacement', {id:j.id}), 'danger'));
        if (clearFinished && ['complete','cancelled'].includes(j.state))
            actions.push(actionButton('Rimuovi attività', 'trash', () => libraryCommand('discard', {id : j.id}), 'danger'));
    }
    return actions;
}
function renderJobs() {
    if (!libraryState)
        return;
    if (afterDetailsMotion($('download-list'),renderJobs)) return;
    const out=$('download-list'),query=folded($('activity-query').value.trim()),
          all=libraryState.jobs.filter(job => !query || folded([job.name,modelLabel(job)].join(' ')).includes(query)),
          matches=(job,ignore=null,value=null) => activityFilterView.sources.every(source => {
              const selected=source===ignore ? value==='all' ? [] : [value] : filterSelection(source);
              if (!selected.length || source.hasAttribute('data-filter-order')) return true;
              const token=selected[0];
              return source.id==='activity-source' ? token===job.source : token==='active' ?
                  ['preparing','downloading','verifying','registering'].includes(job.state) : token==='failed' ?
                  jobHasError(job) || job.state==='failed' : token===job.state;
          });
    for (const [index,source] of activityFilterView.sources.entries()) if (!source.hasAttribute('data-filter-order'))
        filterOptions(source.id,activityDefinitions[index].values,value => all.filter(job => matches(job,source,value)).length);
    const jobs=all.filter(job => matches(job)),order=$('activity-order').value,
          collator=new Intl.Collator(formatLocale,{numeric:true}),
          priority=job => job.state==='queued' ? 0 : ['preparing','downloading','verifying','registering'].includes(job.state) ? 1 : job.state==='paused' ? 2 : 3;
    jobs.sort((a,b) => order==='name' ? collator.compare(modelLabel(a),modelLabel(b)) :
        (order==='all' ? priority(a)-priority(b) : 0) || (order==='oldest' ? a.updated-b.updated : b.updated-a.updated) || b.id-a.id);
    activityFilterView.summary(jobs.length,all.length);
    const retained = new Set(), existing = new Map([...out.children ].map(n => [n.dataset.job, n]));
    let position = out.firstChild;
    for (const j of jobs) {
        const phase = j.phase,
              signature = JSON.stringify([ j.id, j.name, j.state, j.error, j.source, phase, language ]),
              old = existing.get(String(j.id));
        let row = old?.jobSignature === signature ? old.jobRow : null;
        if (!row) {
            row = libraryRow(modelLabel(j), '', () => {
                selectedJob = j.id;
                renderJobs();
                updateLibraryDetails(true);
            }, j.id === selectedJob);
            const badge = make('span', jobLabel(j), 'badge');
            badge.dataset.state = phase;
            badge.prepend(icon(phase === 'complete' ? 'check' : phase === 'registration_failed' ?
                               'triangle-alert' : phase === 'cancelled' ? 'x' : 'download'));
            row.primary.querySelector('h3').append(badge);
            const progress = make('div', '', 'meter-track download-progress');
            progress.setAttribute('role', 'progressbar');
            progress.append(make('i'));
            const previousMeter = old?.querySelector('.download-progress');
            row.primary.append(previousMeter || progress);
            row.actions.append(...jobActions(j));
            if (jobHasError(j)) {
                const feedback = make('span');
                setFeedback(feedback,jobError(j),feedbackKind(j.error));
                row.primary.append(feedback);
            }
            row.card.dataset.job = j.id;
            row.card.jobSignature = signature;
            row.card.jobRow = row;
            if (old) {
                if (position === old)
                    position = row.card;
                old.replaceWith(row.card);
            }
        }
        row.card.classList.toggle('selected', j.id === selectedJob);
        const stamp = make('time');
        setTimestamp(stamp, j.updated);
        row.primary.querySelector('small').replaceChildren([
            tr(j.source === 'ollama' ? 'Ollama'
               : j.source === 'component' ? 'Motore locale' : j.source === 'hf'   ? 'Hugging Face'
                                     : 'URL diretto'),
            jobProgressText(j)
        ].filter(Boolean).join(' · ') + ' · ', stamp);
        const bar = row.primary.querySelector('.download-progress');
        bar.setAttribute('aria-label', jobLabel(j) + ' ' + j.name);
        setMeter(bar, jobFraction(j));
        bar.dataset.indeterminate = String(jobFraction(j) === null && ['preparing', 'downloading', 'verifying'].includes(j.state));
        bar.hidden = ['complete', 'cancelled', 'registration_failed'].includes(phase);
        retained.add(row.card);
        if (position !== row.card)
            out.insertBefore(row.card, position);
        position = row.card.nextSibling;
    }
    for (const node of [...out.childNodes])
        if (!retained.has(node))
            node.remove();
    if (!jobs.length)
        out.append(emptyModel(
            tr(libraryState.jobs.length ? 'Nessun risultato con questi filtri.' : 'Nessun download.')));
}
function modelWarnings(m) {
    const warnings = [];
    if (!m.available)
        warnings.push(tr(m.kind === 2 ? 'Modello non disponibile in Ollama. Scaricalo di nuovo o rimuovilo dalla libreria.'
                                      : 'File non disponibile. Ripristina il file o rimuovi il modello dalla libreria.'));
    if (m.metadata?.fileChanged)
        warnings.push(tr('File modificato. Reimportalo o scaricalo di nuovo.'));
    if (m.sameFile > 1)
        warnings.push(tr('Più riferimenti allo stesso file') + ': ' + m.sameFile);
    if (m.possibleDuplicates > Math.max(1, m.sameFile))
        warnings.push(tr('Possibili duplicati') + ': ' + m.possibleDuplicates);
    return warnings;
}
function updateLibraryDetails(open = false) {
    if (!libraryState || typeof window.renderModelDetails !== 'function')
        return;
    window.renderModelDetails(
        modelTab === 'installed' ? [...libraryState.models, ...libraryState.hiddenModels ].find(
                                       m => m.id === selectedLibraryModel ||
                                            !selectedLibraryModel && m.id === libraryState.selected)
        : modelTab === 'activity'
            ? libraryState?.jobs.find(j => j.id === selectedJob) || libraryState?.jobs[0]
        : modelSource === 'ollama'
            ? (catalogVariants.ollama || [])
                  .map(x => ({...x, source : 'ollama'}))
                  .find(x => x.name === selectedOllamaTag)
            : selectedVariant || repositories.find(item => item.name === selectedRepository),
        modelTab);
    if (open)
        window.setDetailsOpen(true);
}
window.overtoneLibrary = state => {
    const previous = libraryState;
    libraryState = state;
    if (pendingImportToken && state.error)
        pendingImportToken = 0;
    if (JSON.stringify(previous?.importCandidate) !== JSON.stringify(state.importCandidate) ||
        previous?.previewing !== state.previewing || previous?.error !== state.error)
        renderImport();
    if (pendingImportToken && !state.importCandidate) {
        pendingImportToken = 0;
        if (!state.error)
            modelPage('installed');
    }
    if (state.downloadCandidate && state.downloadCandidate.token !== shownDownloadToken &&
        route === 'models' && modelTab === 'search' && modelSource === 'url') {
        shownDownloadToken = state.downloadCandidate.token;
        window.confirmDownload(
            state.downloadCandidate,
            extra => {
                libraryCommand('confirmDownload', {token : state.downloadCandidate.token,...extra}, linkDownloadOrigin);
                modelPage('activity');
            });
    }

    if (!previous || JSON.stringify([previous.engines,previous.engineScan,previous.enginePackages,previous.jobs.filter(j => j.component).map(j => [j.id,j.state])]) !==
        JSON.stringify([state.engines,state.engineScan,state.enginePackages,state.jobs.filter(j => j.component).map(j => [j.id,j.state])]))
        renderEngines();
    $('engine-status').textContent = runtimeDescription();
    const failure = state.error && !state.activityError ? state.error :
        state.engine === 'failed' ? state.engineError : state.projectionLimited ? 'library_limited' : '';
    if (!current?.storageFailed && (failure || $('model-error').feedbackRevision == null))
        setFeedback($('model-error'),failure ? libraryErrors[failure] || explain(failure) : '',feedbackKind(failure));
    updateDiscoveryFeedback(state.discovering);
    engineScanFeedback.update(false,!!state.error);
    for (const [kind,controls] of engineRows) {
        const engine = state.engines.find(e => e.kind === kind);
        controls.verifyFeedback.update(engine?.validation === 3,engine?.validation === 2 || !!state.error);
        controls.detectFeedback.update(false,!!state.error);
    }
    $('ollama-search-button').disabled = state.searching;
    $('model-search-button').disabled = state.searching;
    $('model-search-form').setAttribute('aria-busy', String(state.searching && state.catalogKind === 'search'));
    $('ollama-search-form').setAttribute('aria-busy', String(state.searching && state.catalogKind === 'ollamaSearch'));
    syncCatalogSearchLabels();
    $('installed-count').textContent = state.models.length;
    $('activity-count').textContent = state.jobs.length;
    for (const [id, count] of [['installed', state.models.length], ['activity', state.jobs.length]]) {
        const compact = $('compact-' + id + '-count');
        compact.textContent = count;
        compact.hidden = !count;
    }
    if (!previous ||
        JSON.stringify([ state.models, state.hiddenModels, state.selected, state.busy ]) !==
            JSON.stringify([ previous.models, previous.hiddenModels, previous.selected, previous.busy ]))
        renderInstalled();
    const catalogState = snapshot => [snapshot.catalogKind,snapshot.catalogRepository,snapshot.catalogHasMore,
        snapshot.searching,snapshot.error,snapshot.catalog,snapshot.models,snapshot.busy,snapshot.jobs.map(j => [j.id,j.state])];
    if (!previous || JSON.stringify(catalogState(previous)) !== JSON.stringify(catalogState(state))) {
        renderCatalog();
        renderOllamaCatalog();
    }
    if (!previous || JSON.stringify(state.jobs) !== JSON.stringify(previous.jobs))
        renderJobs();
    updateLibraryDetails();
    window.updateModelMemory?.();
    window.updateShell?.();
    window.updateSetup?.();
    window.updateModelAdvice?.();
    window.controls?.();
    if (downloadOrigin) {
        if (state.jobs.some(j => j.name === downloadOrigin.name &&
                    ['queued','preparing','downloading','verifying','registering','complete'].includes(j.state) &&
                    (!downloadOrigin.states.has(j.id) || downloadOrigin.states.get(j.id) === 'cancelled'))) {
            animateDownloadAttention(downloadOrigin.rect);
            downloadOrigin = null;
        } else if (state.error || performance.now() - downloadOrigin.at > 10000)
            downloadOrigin = null;
    }
};
function syncCatalogSearchLabels() {
    for (const source of ['model', 'ollama']) {
        const form = $(source === 'model' ? 'model-search-form' : 'ollama-search-form'),
              busy = form.getAttribute('aria-busy') === 'true', b = $(source + '-search-button');
        const glyph = busy ? 'rotate-ccw' : 'search';
        if (b.querySelector('use')?.getAttribute('href') !== '#i-' + glyph)
            b.replaceChildren(icon(glyph));
        localAccessible(b, busy ? 'Ricerca in corso' : 'Cerca');
    }
}
window.installedLibrarySnapshot=() => ({total:libraryState?.models.length || 0,
    shown:$('model-list').querySelectorAll('.library-row').length,
    ids:[...$('model-list').querySelectorAll('.library-row')].slice(0,1000).map(node => Number(node.dataset.model)),
    ...filterViews.get('installed')?.snapshot()});
window.modelFilterSnapshot=() => [...filterViews.entries()].map(([id,view]) => ({id,...view.snapshot()}));
window.refreshLibrary = () => {
    for (const select of document.querySelectorAll('select[data-catalog-label]')) {
        select.setAttribute('aria-label',tr(select.dataset.catalogLabel));
        for (const option of select.options) option.textContent = tr(option.dataset.label);
    }
    renderEngines();
    renderInstalled();
    renderCatalog();
    renderOllamaCatalog();
    renderJobs();
    modelPage(modelTab);
    syncCatalogSearchLabels();
};
function updateInstalledFilters(){installedLimit=40;renderInstalled();}
$('installed-query').addEventListener('input',updateInstalledFilters);
$('activity-query').addEventListener('input',renderJobs);
installedFilterView=filterPanel('installed',installedDefinitions,updateInstalledFilters);
const activityDefinitions=[
    {id:'status',label:'Stato attività',icon:'download',values:[['all','Tutti gli stati'],['active','In corso'],['queued','In coda'],['paused','In pausa'],['failed','Richiedono attenzione'],['complete','Completato'],['cancelled','Annullato']]},
    {id:'source',label:'Origine attività',icon:'server',values:[['all','Tutte le origini'],['component','Motori'],['hf','Hugging Face'],['gguf','URL diretto'],['ollama','Ollama']]},
    {id:'order',label:'Ordina per',icon:'list-filter',order:true,values:[['all','Priorità'],['recent','Più recenti'],['oldest','Meno recenti'],['name','Nome']]}];
const activityFilterView=filterPanel('activity',activityDefinitions,renderJobs);
const variantDefinitions=[
    {id:'state',label:'Disponibilità',icon:'circle-check',values:[['all','Tutte'],['installed','Installate'],['new','Non installate'],['acquiring','In download']]},
    sizeFacet,contextFacet,precisionFacet,
    {id:'resources',label:'Risorse',icon:'memory-stick',values:[['all','Tutte'],['fits','Entro il budget'],['limited','Oltre il budget'],['unknown','Da valutare']]},
    {id:'features',label:'Verifica del modello',icon:'shield-check',multiple:true,values:[['verified','Verificati OverTone']]},
    {id:'execution',label:'Esecuzione',icon:'cpu',values:[['all','Tutte'],['local','Locale'],['cloud','Cloud']]},
    {...nameOrder,values:[['all','Nome'],['source','Ordine del catalogo'],['small','Più piccoli'],['large','Più grandi']]}
];
for (const source of ['hf','ollama']) {
    const definitions=[
        {id:'task',label:source==='hf' ? 'Tipo di modello' : 'Capacità',icon:'brain',values:source==='hf' ?
            [['all','Tutti'],['text-generation','Testo e chat'],['image-text-to-text','Immagini e testo'],['feature-extraction','Embedding']] :
            [['all','Tutte'],['thinking','Con ragionamento'],['vision','Immagini'],['tools','Tool'],['embedding','Embedding'],['cloud','Cloud']]},
        ...(source==='hf' ? [
            {id:'max-parameters',bound:'max',label:'Parametri totali · massimo',icon:'cpu',values:[['all','Nessun limite'],...[1,3,7,14,32,70,120].map(n => [String(n),n+'B'])]},
            {id:'license',label:'Licenza',icon:'scale',values:licenseValues.slice(0,5)}] : []),
        {id:'order',label:'Ordina per',icon:'list-filter',order:true,values:[['all',source==='hf' ? 'Rilevanza' : 'Più popolari'],...(source==='hf' ? [['downloads','Più scaricati']] : []),['recent','Più recenti'],...(source==='hf' ? [['trending','Di tendenza']] : [])]},purposeFacet];
    filterPanel(source,definitions,event => {
        if (event.target.id===source+'-purpose') renderModelCatalog(source);
        else searchModels(source);
    },{query:source==='hf' ? 'model-query' : 'ollama-query'});
    const title=$(source==='hf' ? 'variant-title' : 'ollama-variant-title'),header=make('div','','variant-heading flow-anchor');
    const progress = make('span','','variant-progress');
    progress.setAttribute('role','status');
    progress.append(icon('progress-ring'),make('span','','sr-only'));
    title.setAttribute('data-tip-overflow','');
    title.before(header);header.append(title,progress);
    filterPanel(source+'-variant',variantDefinitions,() => renderModelCatalog(source),{query:null,host:header});
    const toggle=$(source+'-variant-filters-toggle');localAccessible(toggle,'Filtra varianti');
}
function selectModelSource(source) {
    window.updateAppMenu?.();
    if (modelSource !== source) closeFloatingSurfaces();
    clearTimeout(modelSearchTimer);
    modelSource = source;
    const catalog = ['hf','ollama'].includes(source), changed = catalogSource !== source;
    $('catalog-scopes').hidden = !catalog;
    $('catalog-back').hidden = catalog;
    $('model-source-heading').hidden = catalog;
    $('model-source-heading').textContent = tr(source === 'url' ? 'Scarica da link' : 'Importa file');
    if (catalog) {
        catalogSource = source;
        selectionChange($('catalog-scopes'), () => {
            for (const button of $('catalog-scopes').querySelectorAll('[data-catalog]')) {
                const selected = button.dataset.catalog === source;
                button.setAttribute('aria-selected', String(selected));
                button.tabIndex = selected ? 0 : -1;
            }
        }, '[aria-selected=true]');
    }
    for (const page of document.querySelectorAll('[data-source-page]'))
        page.hidden = page.dataset.sourcePage !== source;
    if (catalog && changed)
        searchModels(source);
    window.updateCatalogPolicy();
    updateLibraryDetails();
}
for (const button of document.querySelectorAll('[data-catalog]'))
    button.onclick = () => selectModelSource(button.dataset.catalog);
$('catalog-back').onclick = () => selectModelSource(catalogSource);
$('acquire-file').onclick = () => {
    hideTip();
    selectModelSource('file');
    $('import-from-source').click();
};
$('acquire-url').onclick = () => {
    hideTip();
    selectModelSource('url');
    $('download-url').focus();
};

let downloadAttentionTimer = 0, downloadAttentionRevision = 0;
async function animateDownloadAttention(origin) {
    const revision = ++downloadAttentionRevision, area = route, width = innerWidth, height = innerHeight;
    const target = $('download-indicator');
    downloadNoticeUntil = performance.now() + 3000;
    clearTimeout(downloadNoticeTimer);
    downloadNoticeTimer = setTimeout(() => window.updateShell?.(), 3010);
    window.updateShell?.();
    // The flight must be visible, not hidden under a closing confirmation or
    // arriving before its destination has made room in the title bar.
    const pending = [...document.querySelectorAll('dialog[data-closing]')]
        .map(dialog => dialog.dialogMotion?.finished).filter(Boolean);
    pending.push(...(downloadMotion?.effects || []).map(effect => effect.finished));
    await Promise.allSettled(pending);
    if (revision !== downloadAttentionRevision || !target.classList.contains('visible')) return;
    clearTimeout(downloadAttentionTimer);
    target.dataset.attention = 'true';
    downloadAttentionTimer = setTimeout(() => {
        downloadAttentionTimer = 0;
        delete target.dataset.attention;
    }, motionTiming('attention').duration + 50);
    if (!origin || !motionEnabled() || route !== area || innerWidth !== width || innerHeight !== height ||
        document.querySelector('dialog[open]'))
        return;
    const bounds = target.getBoundingClientRect(), end = {left : bounds.right - 36, top : bounds.top, width : 36, height : 36}, marker = document.createElement('span');
    marker.className = 'download-flight';
    const x = origin.left + origin.width / 2, y = origin.top + origin.height / 2;
    marker.style.left = x + 'px';
    marker.style.top = y + 'px';
    document.body.append(marker);
    const animation = marker.animate(
        [
            {opacity : .4, transform : 'translate(-50%,-50%) scale(.6)'},
            {opacity : 1, offset : .2, transform : 'translate(-50%,-50%) scale(1)'}, {
                opacity : .9,
                transform : `translate(${end.left + end.width / 2 - x - 6}px,${
                    end.top + end.height / 2 - y - 6}px) scale(.7)`
            }
        ],
        {...motionTiming('transfer'), fill : 'forwards'});
    measureMotion('transfer',animation);
    const cancel = () => animation.cancel();
    window.addEventListener('resize',cancel,{once:true});
    animation.finished.catch(() => {}).finally(() => {
        window.removeEventListener('resize',cancel);
        marker.remove();
    });
}
