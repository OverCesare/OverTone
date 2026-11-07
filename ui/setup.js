'use strict';
// Guided entry points use the same library commands, settings and acquisition queue.
const configuration = {step:0, option:null, target:'', reuse:null, closing:false, checked:false,
                       existingLimit:20,signature:'',purposes:'',kind:2,scope:'model',commitKey:'',catalogRequested:false,selectionKey:'',intent:'',permission:null,permissionRevision:0,permissionScopes:7};
const purposes=modelPurposes;
const closePurposeMenu = bindOptionMenu($('setup-purpose-menu'),$('setup-purposes')),
      closeEngineMenu = bindOptionMenu($('setup-engine-menu'),$('setup-engines')),
      closeSizeMenu = bindOptionMenu($('setup-size-menu'),$('setup-sizes'));
function finishConfiguration(status = 'skip', dismissParents = status === 'finish') {
    configuration.closing = true;
    libraryCommand('setup',{mode:status});
    const dialog = $('setup-dialog');
    if (status === 'finish') dialog.addEventListener('close', () => {
        navigate('chat');
        $('new').click();
        if (!current.preferences.guideSeen) window.overtoneGuide?.('intro');
    }, {once:true});
    closeDialog(dialog, dismissParents);
}
function openConfiguration(step = 0) {
    configuration.existingLimit = 20;
    $('setup-existing-query').value = '';
    configuration.step = !current?.preferences.setupPaused && (current?.preferences.setupTarget || current?.preferences.setupTargetEngines) ? 2 : step;
    configuration.target = '';
    configuration.intent = current?.preferences.setupTarget || '';
    configuration.selectionKey = '';
    configuration.permission = null;
    configuration.option = null;
    configuration.reuse = null;
    configuration.signature = '';
    configuration.kind = 2;
    configuration.scope = libraryState?.setupModel?.enginesOnly ? 'engines' : 'model';
    configuration.commitKey = configurationKey(current.preferences);
    configuration.catalogRequested = false;
    configuration.closing = false;
    $('setup-consent').checked = networkAllowed(7);
    openDialog($('setup-dialog'));
    libraryCommand('setup',{mode:'open'});
    updateConfiguration();
}
window.openConfiguration = openConfiguration;
for (const control of document.querySelectorAll('[data-open-setup]'))
    control.onclick = () => openConfiguration();
$('close-setup').onclick = $('skip-setup').onclick = () => finishConfiguration();
$('setup-dialog').addEventListener('close', () => {
    if (!configuration.closing && !$('setup-dialog').dataset.restoring)
        libraryCommand('setup',{mode:'skip'});
    configuration.closing = false;
});
function configurationStep(step) {
    if (configuration.step===1 && step!==1) libraryCommand('setup',{mode:'cancelSuggestions'});
    if ([2,4].includes(configuration.step) && [0,1,3].includes(step)) libraryCommand('setup',{mode:'back'});
    closePurposeMenu(); closeEngineMenu(); closeSizeMenu();
    configuration.step = step;
    configuration.signature = '';
    updateConfiguration();
    const surface = document.querySelector('#setup-dialog .configuration-content');
    surface.scrollTop = 0;
    enterSurface(surface);
}
function configurationKey(p) { return [p.purpose,p.setupEngines,p.adviceSizeGB].join(':'); }
function followConfiguration(setup) {
    if (!networkAllowed(setup.resumeAccess || 0)) {
        configuration.scope=setup.enginesOnly ? 'engines' : 'model';
        configuration.option=setup.target;configuration.permission=null;
        configurationStep(1);return;
    }
    configuration.scope=setup.enginesOnly ? 'engines' : 'model';
    configuration.intent=setup.target;configuration.target=setup.name;configuration.kind=setup.kind;
    configurationStep(2);
    libraryCommand('setup',{mode:'follow',target:setup.target,engines:setup.engines});
}
function chooseLocalSetup() {
    configuration.scope=libraryState?.setupModel?.enginesOnly ? 'engines' : 'model';
    configuration.intent=current.preferences.setupTarget || '';
    configuration.catalogRequested=false;configuration.permission=null;
    libraryCommand('setup',{mode:'choose'});configurationStep(1);
}
$('setup-local').onclick = chooseLocalSetup;
for (const control of $('setup-engines').querySelectorAll('[data-engine]')) control.onclick = () => {
    const engines = current.preferences.setupEngines ^ Number(control.dataset.engine);
    if (!engines) return;
    if (configuration.reuse && !(engines & configuration.reuse.kind)) configuration.reuse = null;
    configuration.option = null;
    configuration.signature = '';
    libraryCommand('setup',{mode:'engines',engines});
};
$('setup-existing-query').oninput = () => { configuration.existingLimit=20; updateConfiguration(); };
$('setup-existing').onclick = () => {libraryCommand('setup',{mode:'choose'});configurationStep(3);};
$('setup-back').onclick = () => {
    configuration.reuse = null;
    configurationStep(0);
};
$('setup-import').onclick = () => {
    $('setup-dialog').addEventListener('close', () => {
        navigate('models');
        modelPage('search');
        selectModelSource('file');
        libraryCommand('pickModel');
    }, {once:true});
    finishConfiguration('skip',true);
};
function configureServer() {
    configuration.target = '';
    configuration.kind = 0;
    configuration.scope = 'server';
    prepareConnection();
    openDialog($('settings'));
}
$('setup-server').onclick = $('setup-compatible').onclick = configureServer;
window.configurationServerReady = () => {
    if (!$('setup-dialog').open) return;
    configuration.target = '';
    configuration.kind = 0;
    configuration.scope = 'server';
    configurationStep(4);
};
function modelOptionDetails(option) {
    const body = metadata([
        ['File',option.kind === 1 ? option.metadata?.fileName || option.fileName || option.file || option.name : undefined],
        ['Motore',option.kind === 1 ? 'llama.cpp' : 'Ollama'], ['Contesto',option.context ? countFormat.format(option.context)+' token' : undefined],
        ['Input massimo',option.input ? countFormat.format(option.input)+' token' : undefined],
        ['Output massimo',option.output ? countFormat.format(option.output)+' token' : undefined],
        ['Memoria',option.memory ? '≈ '+sizes(option.memory) : undefined],
        ['Download',option.installed ? tr('Installato') : '≈ '+sizes(option.bytes)],
        ['Licenza',option.license]
    ].filter(row => row[1] !== undefined));
    actionDialog(modelLabel(option),body,option.sourceURL ? [button('Origine',() =>
        send({command:'openLink',url:option.sourceURL}))] : []);
}
function modelChoices(host, options, selected, choose) {
    const shortlist = options.slice(0,3), chosen = options.find(o => o.option === selected);
    if (chosen && !shortlist.includes(chosen)) shortlist[shortlist.length-1] = chosen;
    const identity = shortlist.map(o => o.option).join('\n'), changed = host.configurationIdentity !== identity;
    host.configurationIdentity = identity;
    host.replaceChildren(...shortlist.map((option,index) => {
        const wrapper = make('div','','model-choice'), control = button('',() => choose(option));
        control.className = 'choice-card';
        control.id = host.id+'-choice-'+index;
        control.configurationOption = option.option;
        control.setAttribute('aria-pressed',String(selected === option.option));
        const title = make('strong',modelLabel(option));
        if (option.verified) title.append(verifiedModelMark());
        control.append(icon('box'),title);
        if (option.recommended) {
            const badge = localLabel('span','Consigliato','recommended-badge');
            badge.prepend(icon('thumbs-up'));
            control.append(badge);
        }
        control.append(make('span',option.kind === 1 ? 'llama.cpp' : 'Ollama','configuration-engine'));
        const facts = make('small','','choice-facts');
        if (option.installed) {
            facts.append(icon('circle-check'),localLabel('span','Installato'));
        } else if (!option.fits || option.space === false) {
            facts.append(icon('triangle-alert'),localLabel('span',option.space === false
                ? 'Spazio insufficiente' : 'Memoria insufficiente'));
        } else facts.append(icon('download'),make('span',(option.approximate ? '≈ ' : '')+sizes(option.bytes)));
        control.append(facts);
        const info = actionButton('Dettagli modello','info',() => modelOptionDetails(
            libraryState?.recommendations.find(o => o.option === option.option) || option));
        info.classList.add('choice-info');
        wrapper.append(control,info);
        return wrapper;
    }));
    if (changed && host.childElementCount) enterSurface(host);
}
function configurationJob(job) {
    const row = make('section','','setting-card acquisition-card'), heading = make('div','','row');
    heading.append(icon(job.component ? 'cpu' : 'box'),make('strong',modelLabel(job)));
    const status = make('span',job.state === 'complete' ? tr('Installato') : jobLabel(job),'acquisition-state');
    if (job.state === 'complete') status.prepend(icon('circle-check'));
    status.dataset.complete = String(job.state === 'complete');
    heading.append(status);
    const actions = make('div','','row-actions');
    if (job.id) actions.append(...jobActions(job,false));
    heading.append(actions);
    row.append(heading,make('small',jobProgressText(job),'muted'));
    if (job.state !== 'cancelled') {
        const progress = make('div','','meter-track download-progress');
        progress.setAttribute('role','progressbar');
        progress.setAttribute('aria-label',jobLabel(job)+' '+job.name);
        progress.append(make('i'));
        setMeter(progress,jobFraction(job));
        progress.dataset.indeterminate = String(jobFraction(job) === null && ['preparing','downloading'].includes(job.state));
        row.append(progress);
    }
    if (jobHasError(job)) {
        const feedback = make('p');
        setFeedback(feedback,jobError(job),feedbackKind(job.error));
        row.append(feedback);
    }
    return row;
}
function updateConfiguration() {
    if (!current || !libraryState) return;
    for (const control of document.querySelectorAll('[data-open-setup]'))
        control.hidden = current.preferences.setup === 2;
    if (!configuration.checked && !libraryState.discovering) {
        configuration.checked = true;
        if ((current.preferences.setup === 3 || !current.preferences.setup && !current.ready) &&
            (!libraryState.requested || current.preferences.setup === 3))
            openConfiguration();
    }
    if (!$('setup-dialog').open || $('setup-dialog').hasAttribute('data-closing')) return;
    const p = current.preferences;
    let options = configuration.reuse ? [{...configuration.reuse,option:'reuse',installed:configuration.reuse.id,verified:configuration.reuse.verifiedChat,license:configuration.reuse.metadata?.license,fits:true}] : networkAllowed(1,p) ? libraryState.recommendations || [] : [];
    const committed = libraryState.setupModel,
          setup = configuration.scope !== 'server' && (!configuration.intent || configuration.intent === p.setupTarget) &&
              (configuration.scope === 'engines') === !!committed?.enginesOnly ? committed : null,
          target = setup?.name || configuration.target,
          committedJob = libraryState.jobs.find(j => j.id === committed?.job),
          preserve = !!committed && !committed.enginesOnly && configuration.commitKey === configurationKey(p);
    if (preserve && !libraryState.suggesting && !options.length &&
        (committedJob || committed.id)) {
        const model = libraryState.models.find(m => m.id === committed.id);
        options = [{...model,option:committed.target,name:committed.name,kind:committed.kind,
            installed:committed.id || undefined,bytes:model?.bytes || committedJob?.total || 0,
            fits:true,currentJob:committedJob},...options];
    }
    if (setup && (configuration.step === 2 || configuration.step === 4)) {
        configuration.target = target;
        configuration.kind = setup.kind;
    }
    if (current.revision > configuration.permissionRevision && (configuration.permission ? networkAllowed(configuration.permissionScopes,p) : !p.internet)) configuration.permission = null;
    const enginesOnly = configuration.scope === 'engines', engineKinds = [2,1].filter(kind => ((configuration.step === 2 || configuration.step === 4) && setup ? setup.engines : p.setupEngines) & kind),
          enginesReady = engineKinds.every(kind => libraryState.engines.some(e => e.kind === kind)),
          ready = enginesOnly ? !!setup?.enginesReady : current.ready && (!target || !configuration.kind || enginesReady) &&
                  (!target || !configuration.kind || !!libraryState.selected &&
                    ['ready','asleep'].includes(libraryState.engine) && activeLibraryModel()?.kind === configuration.kind &&
                    activeLibraryModel()?.id === setup?.id), step = configuration.step;
    const selectionKey = [p.purpose,p.setupEngines,p.adviceSizeGB,libraryState.adviceRequest,
        libraryState.adviceUpdated,options.map(o => o.option).join(',')].join(':');
    if (step === 1 && libraryState.suggestionsReady && !libraryState.suggesting && selectionKey !== configuration.selectionKey) {
        configuration.selectionKey = selectionKey;
        configuration.option = (preserve ? options.find(o => o.option === committed.target || o.installed && o.installed === committed.id)?.option : null) ?? options.find(o => o.recommended)?.option ?? options[0]?.option ?? null;
    }
    const selected = configuration.option ?? options.find(o => o.recommended)?.option ?? options[0]?.option,
          targetInstalled = enginesOnly ? !!setup?.enginesReady : !!setup?.id && enginesReady;
    $('skip-setup').textContent=tr('Più tardi');
    for (const [id,show] of [['setup-start',step===0],['setup-choose',step===1],
                             ['setup-work',step===2],['setup-reuse',step===3],['setup-summary',step===4]]) $(id).hidden = !show;
    $('setup-heading').textContent = step===1 ? tr('Un modello per {device}').replace('{device}',
        document.documentElement.dataset.device || tr('questo computer')) :
        tr(step===0 ? 'Inizia con OverTone' : step===3 ? 'Usa ciò che hai già' : step===4 ? ready ? 'Pronto' : enginesOnly ? 'Preparazione' : 'Preparazione del modello' : 'Preparazione');
    $('setup-back').hidden = step===0;
    $('setup-existing').hidden = step!==0;
    $('setup-engines-only').hidden = step!==1;
    $('setup-engines-only').disabled = !!current.storageFailed;
    const primary = $('setup-action');
    primary.hidden = step===0 || step===3 || step===2 && !ready;
    primary.disabled = !!current.storageFailed || step===4 && !ready;
    if (step===2 && (ready || targetInstalled)) { configurationStep(4); return; }
    if (step===0 && committed) {
        primary.hidden=false;primary.textContent=tr(p.setupPaused ? 'Riprendi preparazione' : 'Mostra preparazione');primary.onclick=() => followConfiguration(committed);
    }
    if (step===4) {
        const model = enginesOnly ? null : libraryState.models.find(m => m.id === setup?.id) || activeLibraryModel(),
              c = current.connection, summary = $('setup-summary'), failed = !enginesOnly && libraryState.engine === 'failed',
              data = model ? {...model,context:libraryState.selected === model.id ? p.context : undefined,local:true,ready,
                  phase:!ready ? tr(failed ? 'Non disponibile' : 'Caricamento') : undefined} :
                  {name:c.displayName || c.model,quantization:c.quantization,context:p.context,
                   engine:c.ollama ? 'Ollama' : tr('OpenAI compatibile'),serverConnection:true,local:!c.remote,ready};
        const signature = JSON.stringify([enginesOnly,engineKinds,language,modelLabel(data),data.verifiedChat,data.kind,data.context,
            data.engine,data.serverConnection,data.local,data.ready,data.phase,failed]);
        if (summary.dataset.summary !== signature) {
            summary.dataset.summary=signature;
            const card=enginesOnly ? metadata(engineKinds.map(kind => [kind===2 ? 'Ollama' : 'llama.cpp',tr('Installato')])) : modelSummaryCard(data);
            if (!ready && !failed) {
                const progress=make('div','','meter-track download-progress');
                progress.dataset.indeterminate='true';progress.setAttribute('role','progressbar');
                progress.setAttribute('aria-label',tr('Preparazione del modello'));progress.append(make('i'));
                card.append(progress);
            }
            summary.replaceChildren(card);
        }
        primary.hidden=false;primary.disabled=!!current.storageFailed || !ready && !failed;
        primary.textContent=tr(failed ? 'Riprova' : 'Inizia');
        primary.onclick=() => failed && model ? libraryCommand('setup',{mode:'use',id:model.id,
            engines:setup?.engines || p.setupEngines,allowDownloads:false}) : ready && finishConfiguration('finish');
    } else if (step===1) {
        for (const control of $('setup-engines').querySelectorAll('[data-engine]')) {
            const kind = Number(control.dataset.engine), selected = !!(p.setupEngines & kind);
            control.setAttribute('aria-pressed',String(selected));
            control.setAttribute('aria-checked',String(selected));
            control.setAttribute('aria-disabled',String(p.setupEngines === kind));
        }
        if (!configuration.reuse && configuration.permission !== false && (networkAllowed(1,p) || configuration.permission === true && (configuration.permissionScopes & 1)) && !configuration.catalogRequested) {
            configuration.catalogRequested = true;
            libraryCommand('setup',{mode:'suggest',allowDownloads:false});
        }
        if (configuration.option === null && options.length && libraryState.suggestionsReady && !libraryState.suggesting)
            configuration.option = selected;
        if (configuration.purposes !== language) {
            configuration.purposes = language;
            $('setup-sizes').replaceChildren(...modelGBChoices.map(gb => {
                const choice = button(gb ? '≈ '+countFormat.format(gb)+' GB' : tr('Nessun limite'),() => {
                    closeSizeMenu(keyboardInput); configuration.option = null;
                    libraryCommand('setup',{mode:'size',gb});
                });
                choice.dataset.gb = gb;choice.setAttribute('role','menuitemradio');return choice;
            }));
            $('setup-purposes').replaceChildren(...purposes.map(([label,glyph],purpose) => {
                const choice = button(label,() => {
                    closePurposeMenu(keyboardInput);
                    configuration.option = null;
                    libraryCommand('setup',{mode:'purpose',purpose});
                });
                choice.prepend(icon(glyph));
                choice.id = 'setup-purpose-'+purpose;
                choice.setAttribute('role','menuitemradio');
                return choice;
            }));
        }
        for (const [purpose,choice] of [...$('setup-purposes').children].entries()) {
            choice.setAttribute('aria-pressed',String(purpose === p.purpose));
            choice.setAttribute('aria-checked',String(purpose === p.purpose));
        }
        for (const choice of $('setup-sizes').children) choice.setAttribute('aria-checked',String(Number(choice.dataset.gb) === p.adviceSizeGB));
        const purpose = purposes[p.purpose] || purposes[0],
              engineNames = engineKinds.map(kind => kind === 2 ? 'Ollama' : 'llama.cpp').join(' + '),
              summary = [language,p.purpose,p.setupEngines,p.adviceSizeGB].join(':');
        if ($('setup-purpose-menu').dataset.summary !== summary) {
            $('setup-purpose-menu').dataset.summary = summary;
            $('setup-purpose-menu').replaceChildren(icon(purpose[1]),make('span',tr(purpose[0])),icon('chevron-down'));
            $('setup-engine-menu').replaceChildren(icon('cpu'),make('span',engineNames),icon('chevron-down'));
            $('setup-size-menu').replaceChildren(icon('hard-drive'),make('span',p.adviceSizeGB ? '≈ '+countFormat.format(p.adviceSizeGB)+' GB' : tr('Nessun limite')),icon('chevron-down'));
            $('setup-size-menu').setAttribute('aria-label',tr('Dimensione modello'));
            $('setup-purpose-menu').setAttribute('aria-label',tr('Uso principale')+': '+tr(purpose[0]));
            $('setup-engine-menu').setAttribute('aria-label',tr('Motori')+': '+engineNames);
        }
        const signature = JSON.stringify([language,options.map(o => [o.option,modelLabel(o),o.bytes,o.installed,o.fits,o.space,o.recommended,o.verified])]);
        if (configuration.signature !== signature) {
            configuration.signature = signature;
            modelChoices($('setup-models'),options,enginesOnly ? null : selected,option => {
                configuration.scope='model';
                configuration.option = option.option;
                updateConfiguration();
            });
        }
        // Preserve controls on selection: CSS transitions, focus and tooltips keep their lifetime.
        for (const choice of $('setup-models').querySelectorAll('.choice-card'))
            choice.setAttribute('aria-pressed',String(!enginesOnly && choice.configurationOption === selected));
        const waiting = libraryState.suggesting, noChoices = !options.length;
        const status = $('setup-suggestions');
        status.hidden = !noChoices && !libraryState.suggestionError;
        if (!status.hidden) {
            const online=configuration.permission === true || networkAllowed(1,p);
            const signature = [language,waiting,online,libraryState.suggestionError].join(':');
            if (status.dataset.status !== signature) {
                status.dataset.status = signature;
                status.replaceChildren(icon(waiting ? 'loader' : 'search'),
                    localLabel('span',waiting ? 'Ricerca dei modelli' : libraryState.suggestionError
                        ? 'Catalogo non disponibile' : online ? 'Nessun modello adatto a queste scelte.' : 'Cataloghi online disattivati.'));
                if (!waiting && libraryState.suggestionError) status.append(button('Riprova',() =>
                    libraryCommand('setup',{mode:'suggest',refresh:true})));
            }
            status.classList.toggle('preparing',waiting);
        }
        const option = options.find(o => o.option === (configuration.reuse ? 'reuse' : selected)),
              needsEngine = !enginesReady,
              follows = !!option && !!committed && !!(committedJob || committed.id) && !committed.enginesOnly && committed.engines===p.setupEngines &&
                  (option.option === committed.target || option.installed && option.installed===committed.id || option.kind===2 && option.name===committed.target);
        const required = follows || enginesOnly && committed?.enginesOnly ? committed.resumeAccess || 0 :
              (option?.installed ? 0 : 3) | (needsEngine ? 4 : 0);
        configuration.permissionScopes=required | (configuration.reuse || enginesOnly ? 0 : 1);
        const allowed=configuration.permission ?? networkAllowed(required,p);
        $('setup-consent').checked=configuration.permission ?? networkAllowed(configuration.permissionScopes,p);
        const permissionRequired = !allowed && (!option?.installed || needsEngine);
        $('setup-consent-row').hidden = !!configuration.reuse && !!option?.installed && !needsEngine;
        $('setup-consent-row').classList.toggle('permission-required',permissionRequired);
        primary.hidden = !option && allowed;
        primary.textContent = tr(permissionRequired ? 'Attiva download' : follows ? p.setupPaused ? 'Riprendi preparazione' : 'Mostra preparazione' : option?.installed ? needsEngine ? 'Prepara e avvia' : 'Usa modello' : 'Scarica e avvia');
        primary.disabled ||= !permissionRequired && (!option || !option.installed && (!option.fits || option.space === false));
        primary.onclick = () => {
            if (permissionRequired) {
                $('setup-consent').checked=true;$('setup-consent').dispatchEvent(new Event('change'));return;
            }
            if (!option) return;
            configuration.scope = 'model';
            configuration.commitKey = configurationKey(p);
            if (follows) {
                followConfiguration(committed);return;
            }
            configuration.intent = option.installed ? 'local:'+option.installed : option.kind === 1 ? option.option : option.name;
            configuration.target = option.name;
            configuration.kind = option.kind || 2;
            const origin=primary.getBoundingClientRect();
            configurationStep(2);
            libraryCommand('setup',option.installed ? {mode:'use',id:option.installed,allowDownloads:false,name:option.name} :
                           {mode:'prepare',kind:option.kind,allowDownloads:false,name:option.name},origin);
        };
        if (enginesOnly && committed?.enginesOnly) {
            primary.hidden=committed.engines!==p.setupEngines;
            primary.disabled=!!current.storageFailed;
            primary.textContent=tr(permissionRequired ? 'Attiva download' : p.setupPaused ? 'Riprendi preparazione' : 'Mostra preparazione');
            primary.onclick=() => {
                if (permissionRequired) {$('setup-consent').checked=true;$('setup-consent').dispatchEvent(new Event('change'));return;}
                followConfiguration(committed);
            };
        }
    } else if (step===3) {
        const query = folded($('setup-existing-query').value.trim());
        const signature = JSON.stringify([language,libraryState.models,libraryState.pending,libraryState.engine,query,configuration.existingLimit]);
        if (configuration.signature !== signature) {
            configuration.signature = signature;
            const models = libraryState.models.filter(m => !m.hidden && m.available && !m.metadata?.fileChanged &&
                !m.metadata?.cloud && m.metadata?.chatSupported !== false && folded(modelLabel(m)).includes(query));
            const nodes = models.slice(0,configuration.existingLimit).map(model => {
                const row = libraryRow(modelLabel(model),model.kind===2 ? 'Ollama' : 'llama.cpp',() => {
                    configuration.scope='model';configuration.intent='local:'+model.id;
                    if (!libraryState.engines.some(e => e.kind === model.kind)) {
                        configuration.reuse = model;
                        libraryCommand('setup',{mode:'engines',engines:model.kind});
                        configurationStep(1);
                        return;
                    }
                    libraryCommand('setup',{mode:'use',id:model.id,engines:model.kind,allowDownloads:false});
                    configuration.intent = 'local:'+model.id;
                    configuration.target = model.name;
                    configuration.kind = model.kind;
                    configurationStep(2);
                });
                row.primary.querySelector('h3').prepend(icon('box'));
                row.card.dataset.kind = model.kind; row.card.dataset.modelId = model.id;
                const arrow = icon('chevron-right'); arrow.classList.add('library-disclosure');
                row.actions.append(arrow);
                return row.card;
            });
            if (models.length > configuration.existingLimit) nodes.push(button('Carica altri modelli', () => {
                const surface = document.querySelector('#setup-dialog .configuration-content'), top = surface.scrollTop;
                configuration.existingLimit += 20;
                updateConfiguration();
                surface.scrollTop = top;
            }));
            $('setup-existing-models').replaceChildren(...nodes);
            if (!nodes.length) $('setup-existing-models').append(emptyModel(tr('Nessun modello locale trovato.')));
        }
    } else if (step===2) {
        const local = enginesOnly || !!target && configuration.kind,
              model = libraryState.jobs.find(j => j.id === setup?.job),
              stages = local ? [
                  ...engineKinds.map(kind => {
                      const name = kind === 2 ? 'Ollama' : 'llama.cpp',
                            engine = libraryState.engines.some(e => e.kind === kind),
                            component = libraryState.jobs.find(j => j.component && j.name === name &&
                                (engine ? j.state === 'complete' : j.state !== 'complete'));
                      return ['engine-'+kind,component || {name,component:true,
                          state:engine ? 'complete' : 'preparing',received:0,total:0}];
                  }),
                  ...(!enginesOnly ? [['model',model || {name:target,state:ready ? 'complete' : libraryState.engine === 'failed' ? 'failed' : 'preparing',received:0,total:0}]] : [])
              ] : [];
        const host = $('setup-jobs'), retained = new Set(stages.map(([key]) => key));
        for (const row of [...host.children]) if (!retained.has(row.dataset.stage)) row.remove();
        for (const [key,job] of stages) {
            const signature = JSON.stringify([language,job.id,job.name,job.state,job.error]),
                  identity=job.id ? 'job:'+job.id : 'pending:'+job.name,
                  old = host.querySelector('[data-stage="'+key+'"]');
            let row = old;
            if (!row || row.configurationSignature !== signature) {
                row = configurationJob(job);
                const meter = old?.querySelector('.meter-track'), next = row.querySelector('.meter-track');
                if (old?.configurationIdentity===identity && meter && next) next.replaceWith(meter);
                row.configurationIdentity=identity;
                row.configurationSignature = signature; row.dataset.stage = key;
                if (old) old.replaceWith(row); else {host.append(row); enterSurface(row);}
            }
            const facts = row.querySelector(':scope>.muted');
            if (facts) facts.textContent = jobProgressText(job);
            const meter = row.querySelector('.meter-track');
            if (meter) {
                setMeter(meter,jobFraction(job));
                meter.dataset.indeterminate = String(jobFraction(job) === null && ['preparing','downloading'].includes(job.state));
            }
        }
        const missing = !enginesOnly && setup && !setup.id && (!model || model.state === 'complete');
        if (missing) {
            primary.hidden=false;primary.disabled=false;primary.textContent=tr('Scegli modello');
            primary.onclick=chooseLocalSetup;
        } else primary.hidden = !ready && !!local;
        if (!missing) primary.textContent = tr(ready ? 'Inizia' : 'Collega un server');
        if (!missing) primary.onclick = ready ? () => finishConfiguration('finish') : () => {
            prepareConnection(); openDialog($('settings'));
        };
    }
    const error = !enginesOnly && configuration.step === 2 && setup && !setup.id &&
        !libraryState.jobs.some(j => j.id === setup.job && j.state !== 'complete') ? 'not_found' :
        libraryState.activityError ? '' : libraryState.error || (enginesOnly ? '' : libraryState.engineError);
    setFeedback($('setup-error'),error ? libraryErrors[error] || explain(error) : '',feedbackKind(error));
}
$('setup-engines-only').onclick = () => {
    const kinds=[2,1].filter(kind => current.preferences.setupEngines & kind),
          missing=kinds.filter(kind => !engineAvailable(kind));
    function prepare(allowDownloads=false) {
        configuration.scope='engines';configuration.target='';configuration.intent='';configuration.kind=0;
        configuration.commitKey=configurationKey(current.preferences);
        configurationStep(2);
        libraryCommand('setup',{mode:'enginesOnly',allowDownloads});
    }
    if (!missing.length || networkAllowed(4)) {prepare();return;}
    const body=make('div','','settings-fields'), consent=make('input');consent.type='checkbox';
    body.append(metadata(missing.map(kind => {
        const entry=libraryState.enginePackages.find(p => p.kind===kind);
        return [entry.name,sizes(entry.bytes)];
    })));
    const label=make('label','','check');label.append(localLabel('span','Consenti accesso a Internet'),consent);body.append(label);
    const install=button('Installa',() => {closeDialog($('action-dialog'));prepare(true);});
    install.id='confirm-setup-engines';install.className='primary';install.disabled=true;
    consent.id='setup-engine-consent';consent.onchange=() => {install.disabled=!consent.checked;};
    actionDialog('Solo motori',body,[install]);
};
$('setup-consent').onchange = () => {
    configuration.permission = $('setup-consent').checked;
    configuration.permissionRevision = current.revision;
    configuration.catalogRequested = false;
    setCatalogueAccess(configuration.permission,configuration.permissionScopes);
    updateConfiguration();
};
window.updateSetup = updateConfiguration;
// Bounded, content-free geometry for the existing opt-in local control interface.
window.configurationSnapshot = () => {
    const dialog = $('setup-dialog');
    if (!dialog.open) return {open:false};
    const content = dialog.querySelector('.configuration-content'), bounds = dialog.getBoundingClientRect(),
          automatic = (libraryState?.recommendations || []).find(o => o.recommended)?.option ?? libraryState?.recommendations?.[0]?.option;
    const style = element => {
        if (!element) return {};
        const css = getComputedStyle(element);
        return {ink:css.color,fill:css.backgroundColor,radius:css.borderRadius};
    };
    return {open:true,existing:[...$('setup-existing-models').querySelectorAll('[data-model-id]')].slice(0,40).map(row=>Number(row.dataset.modelId)),existingMore:!!$('setup-existing-models').querySelector(':scope>button'),step:configuration.step,enginesOnly:configuration.scope==='engines',pending:!!(current?.preferences.setupTarget || current?.preferences.setupTargetEngines),held:!!current?.preferences.setupPaused,
        engines:current.preferences.setupEngines,purpose:current.preferences.purpose,sizeGB:current.preferences.adviceSizeGB,device:document.documentElement.dataset.device || '',
        width:bounds.width,height:bounds.height,scrollTop:content.scrollTop,scrollHeight:content.scrollHeight,clientHeight:content.clientHeight,overflow:content.scrollWidth-content.clientWidth,
        viewportOverflow:bounds.top < 0 || bounds.bottom > innerHeight,
        primaryVisible:!hiddenUIElement($('setup-action')),primaryDisabled:$('setup-action').disabled,primaryLabel:$('setup-action').textContent,
        permissionRequired:configuration.step===1 && $('setup-consent-row').classList.contains('permission-required'),
        target:libraryState?.setupModel?.target || configuration.intent,
        suggesting:!!libraryState?.suggesting,
        suggestions:(libraryState?.recommendations || []).slice(0,8).map(o =>
            ({option:o.option,kind:o.kind,bytes:o.bytes,context:o.context,input:o.input,output:o.output,installed:!!o.installed,
              recommended:o.recommended,selected:configuration.step === 1 && o.option === (configuration.option ?? automatic)})),
        choices:[...dialog.querySelectorAll('.choice-card')].filter(b => !hiddenUIElement(b)).slice(0,3)
            .map(b => ({selected:b.getAttribute('aria-pressed')==='true',...style(b),
                label:style(b.querySelector('strong')),note:style(b.querySelector('small')),
                badge:style(b.querySelector('.recommended-badge')),engine:style(b.querySelector('.configuration-engine')),
                info:style(b.parentElement.querySelector('.choice-info'))})),
        stages:configuration.step===2 ? [...$('setup-jobs').children].map(row => ({stage:row.dataset.stage,
            complete:row.querySelector('.acquisition-state')?.dataset.complete === 'true',
            ratio:row.querySelector('[role=progressbar]')?.getAttribute('aria-valuenow'),
            meter:row.querySelector('.meter-track>i')?.style.transform})) : [],
        controls:[...dialog.querySelectorAll('button')].filter(b => !hiddenUIElement(b)).slice(0,32)
            .map(b => {const r=b.getBoundingClientRect();return {id:b.id,width:r.width,height:r.height,
                textOverflow:b.scrollWidth>b.clientWidth+1,outside:r.left<bounds.left || r.right>bounds.right ||
                    !!b.closest('.dialog-heading,.dialog-footer') && (r.top<bounds.top || r.bottom>Math.min(bounds.bottom,innerHeight))};})};
};
function installEngine(kind, continuation = null) {
    const packageInfo = libraryState?.enginePackages?.find(p => p.kind === kind);
    if (!packageInfo) return;
    const body = make('div','','settings-fields');
    body.append(make('p',packageInfo.name+' '+packageInfo.version+' · '+sizes(packageInfo.bytes)));
    const consent = make('input'); consent.type='checkbox'; consent.checked=networkAllowed(4);
    const label = make('label','','check');
    label.append(localLabel('span','Consenti accesso a Internet'),consent);
    label.hidden = networkAllowed(4);
    body.append(label);
    const install = button('Installa',() => {
        const origin = install.getBoundingClientRect();
        closeDialog($('action-dialog'));
        libraryCommand('installEngine',{kind,allowDownloads:consent.checked,name:packageInfo.name},origin);
        continuation?.();
    });
    install.className = 'primary';
    install.id = 'confirm-engine-install';
    install.disabled = !consent.checked;
    consent.onchange = () => { install.disabled = !consent.checked; };
    actionDialog(tr('Installa')+' '+packageInfo.name,body,[install]);
}
