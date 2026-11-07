'use strict';
let detailItem = null, detailKind = '', detailKey = '', detailTab = '', detailSignature = '';
function detailCard(title, glyph, rows, literalTitle = false) {
    const card = make('section', '', 'detail-card'), heading = make('h3', literalTitle ? title : tr(title));
    heading.prepend(icon(glyph));
    card.append(heading, rows instanceof Node ? rows : metadata(rows.filter(x => x[1] !== undefined)));
    return card;
}
function detailMessages(messages) {
    const body=make('div','','detail-notices');
    for (const text of messages) body.append(make('p',text));
    return body;
}
function observedRuntimeRows() {
    const runtime = runtimeObservation();
    return [
        ['Contesto effettivo', runtime?.effectiveContext ? runtime.effectiveContext.toLocaleString(formatLocale) : undefined],
        ['Batch / microbatch', runtime?.batch ? runtime.batch + ' / ' + runtime.microBatch : undefined],
        ['Layer GPU rilevati', runtime?.layersObserved ?
            runtime.gpuLayersObserved + ' / ' + runtime.layersObserved : undefined],
        ['Altri modelli residenti', runtime?.otherResidentModels || undefined]
    ];
}
window.renderModelDetails = (item, kind) => {
    detailItem = item;
    detailKind = kind;
    window.renderDetails();
};
window.renderDetails = () => {
    if (!current)
        return;
    const names = {
        chat : 'Chat',
        models : 'Modelli',
        knowledge : 'Documenti',
        projects : 'Progetti',
        tools : 'Strumenti'
    },
          definition = route === 'models' ? [ [ 'model', 'Dettagli' ] ]
                       : route === 'chat'
                           ? [ [ 'sources', 'Fonti' ], [ 'context', 'Contesto' ], [ 'technical', 'Tecnico' ] ]
                       : route === 'knowledge'
                           ? [ [ 'documents', 'Documenti' ], [ 'index', 'Indice' ], [ 'sources', 'Fonti' ] ]
                       : route === 'projects'
                           ? [ [ 'scope', 'Progetto' ], [ 'memory', 'Memoria' ], [ 'policy', 'Permessi' ] ]
                           : [ [ 'sources', 'Fonti' ], [ 'context', 'Contesto' ], [ 'policy', 'Permessi' ] ],
          key = route + ':' + language;
    if (detailKey !== key) {
        detailKey = key;
        detailTab = definition[0][0];
        $('details-tabs').hidden = definition.length < 2;
        $('details-tabs').replaceChildren();
        for (const [id, label] of definition) {
            const b = button(label, () => {
                const changed = detailTab !== id;
                detailTab = id;
                window.renderDetails();
                if (changed)
                    enterSurface($('details-content'));
            });
            b.setAttribute('role', 'tab');
            b.id = 'detail-tab-' + id;
            b.setAttribute('aria-controls', 'details-content');
            b.dataset.tab = id;
            $('details-tabs').append(b);
        }
    }
    for (const b of $('details-tabs').children) {
        b.setAttribute('aria-selected', String(b.dataset.tab === detailTab));
        b.tabIndex = b.dataset.tab === detailTab ? 0 : -1;
    }
    $('details-title').textContent = tr(route === 'models' ? 'Modelli' : 'Dettagli');
    $('details-subtitle').textContent = route === 'models' && detailItem ? modelLabel(detailItem) : tr(names[route]);
    $('details-content').setAttribute('aria-labelledby', 'detail-tab-' + detailTab);

    if (!detailsOpen)
        return;
    const r = window.lastResources, p = current.preferences, m = activeLibraryModel(),
          item = detailItem ? {...detailItem.metadata, ...detailItem} : null;
    // Hidden inspectors retain the latest sample without scheduling meter transitions.
    // Opening the panel paints that sample before its entrance animation starts.
    if (r) {
        $('memory').hidden = !r.total;
        $('memory-label').textContent = tr(r.unified ? 'Memoria unificata' : 'Memoria');
        setMeter($('memory-bar'), r.total ? r.used / r.total : 0);
        $('memory-value').textContent = `${gib(r.used)} / ${gib(r.total)}`;
    }
    // Byte counters update in place, preserving download controls and keyboard focus.
    const itemStructure = item ? {...item, received : 0, total : 0, bytesPerSecond : 0,
                                  updated : detailKind === 'activity' ? 0 : item.updated} : null,
          observation = runtimeObservation();
    const signature = JSON.stringify([
        key, detailTab, itemStructure, detailKind, language, formatRevision,
        current.selected, current.ready, current.active, current.improving,
        current.error, current.archived, current.deleted, p.context, p.input, p.output, p.keepAlive,
        p.gpuLayers, p.threads, m?.id, libraryState?.engine, libraryState?.engineError, current.head, current.lastCompletion,
        route === 'models' ? [current.thinkingKnown, current.thinkingOptions] : null,
        route === 'models' && item?.id === m?.id ?
            [observation?.acceleration, observation?.effectiveContext, observation?.batch,
             observation?.microBatch, observation?.gpuLayersObserved, observation?.layersObserved,
             observation?.otherResidentModels] : null,
        route === 'chat' && detailTab === 'technical' ? [r, window.modelMemory] : null
    ]);
    const out = $('details-content');
    if (signature !== detailSignature) {
        detailSignature = signature;
        out.replaceChildren();
        if (route === 'models') {
            if (!item)
                out.append(make('p', tr('Seleziona un modello'), 'empty'));
            else if (detailKind === 'installed') {
                const warnings = modelWarnings(item);
                if (warnings.length) out.append(detailCard('Avvisi','triangle-alert',detailMessages(warnings)));
                out.append(detailCard(item.displayName || item.name, 'box', [
                    ['Runtime', item.kind === 1 ? 'llama.cpp' : 'Ollama'],
                    ['Formato', item.kind === 1 ? 'GGUF' : tr('Gestito dal servizio')],
                    ['Dimensione', (item.sizeApproximate ? '≈ ' : '') + sizes(item.bytes)],
                    ['Origine', tr({manual : 'File locale', cache : 'Cache esterna', overtone : 'OverTone', ollama : 'Ollama'}[item.origin] || '—')]
                ], true));
                out.append(detailCard('Installazione', 'hard-drive', [
                    [ 'Stato', tr(item.available ? 'Installato' : 'Non disponibile') ],
                    [
                        'Gestione', tr(item.kind === 2 ? 'Ollama'
                                       : item.managed  ? 'File gestito da OverTone'
                                                       : 'File locale esterno')
                    ],
                    [ 'Licenza', item.license || tr('Da verificare alla fonte') ],
                    [ item.kind === 1 ? 'File' : 'Riferimento', item.kind === 1 ? item.fileName || item.file : item.file ],
                    [ 'Riferimenti allo stesso file', item.kind === 1 && item.sameFile > 1 ? item.sameFile : undefined ]
                ]));
                if (item.family || item.parameterSize || item.quantization || item.context || item.layers) {
                    out.append(detailCard('Caratteristiche', 'box', [
                        [ 'Famiglia', item.family || undefined ],
                        [ 'Parametri', item.parameterSize ],
                        [ 'Quantizzazione', item.quantization ],
                        [ 'Contesto massimo', item.context ? item.context.toLocaleString(formatLocale) + ' token' : undefined ],
                        [ 'Layer', item.layers || undefined ]
                    ]));
                }
                const selected = item.id === m?.id;
                if (selected) {
                    const active = detailCard('Esecuzione', 'cpu', [
                        [ 'Stato', runtimeDescription() ],
                        [ 'Diagnostica', libraryState.engineError ? tr(libraryErrors[libraryState.engineError] || explain(libraryState.engineError)) : undefined ],
                        ...observedRuntimeRows(),
                        [ 'Motore', item.kind === 1 ? 'llama.cpp' : 'Ollama' ]
                    ]);
                    const ready = current.ready && libraryState.engine === 'ready',
                          action = button(ready ? 'Parametri del modello' : 'Riprova', () =>
                              ready ? modelParameters() : libraryCommand('select', {id : item.id}));
                    action.prepend(icon(ready ? 'sliders-horizontal' : 'rotate-ccw'));
                    action.disabled = !ready && (!!current.active || !!current.improving) || !!current.maintenance ||
                                      !!libraryState?.busy;
                    active.append(action);
                    out.append(active);
                } else {
                    const use = button('Usa modello', () => libraryCommand('select', {id : item.id}));
                    use.prepend(icon('check'));
                    use.className = 'primary';
                    use.disabled = ((!item.available || item.fileChanged) && item.kind === 1) || !!current.active || !!current.improving ||
                                   !!current.maintenance || !!libraryState?.busy;
                    out.append(use);
                }
            } else if (detailKind === 'activity') {
                const card = detailCard('Download', 'download', [
                    [
                        'Stato', jobLabel(item)
                    ],
                    ['Origine', item.source === 'hf' ? 'Hugging Face' : item.source === 'ollama' ? 'Ollama' : tr('URL diretto')],
                    ['Creato', formatTimestamp(item.created)],
                    ['Aggiornato', '—', 'job-detail-updated'],
                    ['Gestione', item.source === 'ollama' ? 'Ollama' : 'OverTone'],
                    ['Ricevuti', '—', 'job-detail-received'],
                    ['Dimensione', '—', 'job-detail-total'],
                    ['Velocità', '—', 'job-detail-speed'],
                    ['Registrazione', tr(item.registered ? 'Completata' : 'Da completare')]
                ]);
                out.append(card);
                if (jobHasError(item)) {
                    const body=detailMessages([jobError(item)]);
                    body.append(metadata([['Codice',item.error]]));
                    out.prepend(detailCard('Errore', 'triangle-alert', body));
                }
            } else {
                const variant = !!item.url || item.source === 'ollama' && item.name.includes(':');
                out.append(detailCard(item.repository || (variant && item.source === 'ollama' ? item.name.split(':')[0] : item.name), 'globe', [
                    [ 'Catalogo', item.source === 'ollama' ? 'Ollama' : 'Hugging Face' ],
                    [ 'Licenza', item.license || tr('Da verificare alla fonte') ],
                    [ 'Dimensione', !variant && item.bytes ? sizes(item.bytes) : undefined ],
                    [ 'Download', item.downloads?.toLocaleString(formatLocale) ],
                    [ 'Like', item.likes?.toLocaleString(formatLocale) ],
                    [ 'Aggiornato', item.modified ? formatTimestamp(Date.parse(item.modified) / 1000) : undefined ],
                    [ 'Revisione', item.revision || undefined ]
                ], true));
                if (item.tags?.length)
                    out.append(detailCard('Metadati', 'info', [ [ 'Tag', item.tags.join(', ') ] ]));
                if (variant) {
                    const quantization = item.quantization,
                          parameters = item.parameterSize || item.name.match(/(?:^|[.\-_])(\d+(?:\.\d+)?B)(?:[.\-_]|$)/i)?.[1];
                    out.append(detailCard('Variante selezionata', 'hard-drive', [
                        ['Modello', item.displayName || item.name], ['File', item.source !== 'ollama' ? item.name : undefined], ['Dimensione', item.bytes ? sizes(item.bytes) : undefined],
                        ['Quantizzazione', quantization], ['Parametri', parameters],
                        ['Contesto massimo', item.context ? item.context.toLocaleString(formatLocale) + ' token' : undefined]
                    ]));
                }
                if (item.sourceURL) {
                    const source =
                        button('Apri la fonte', () => send({command : 'openLink', url : item.sourceURL}));
                    source.prepend(icon('external-link'));
                    out.append(source);
                }
            }
        } else if (route === 'chat') {
            if (detailTab === 'sources')
                out.append(detailCard('Fonti', 'library',
                                      [ [ 'Documenti', tr('Predisposto') ], [ 'Web', tr('Non attivo') ] ]));
            else if (detailTab === 'context')
                out.append(detailCard('Contesto', 'memory-stick', [
                    [ 'Modello', modelLabel(m) || current.connection.model || '—' ],
                    [ 'Contesto', p.context.toLocaleString(formatLocale) + ' token' ],
                    [
                        'Input massimo',
                        (p.input || p.context - p.output - 256).toLocaleString(formatLocale) + ' token'
                    ],
                    [ 'Output massimo', p.output.toLocaleString(formatLocale) + ' token' ],
                    [
                        'Memoria conversazione',
                        tr(current.contextTrimmed ? 'Cronologia ridotta al budget' : 'Locale')
                    ]
                ]));
            else {
                out.append(detailCard('Esecuzione', 'cpu', [
                    [ 'Motore', m ? (m.kind === 1 ? 'llama.cpp' : 'Ollama') : tr('API configurata') ],
                    ['Stato', runtimeDescription()],
                    ...observedRuntimeRows(),
                    [ 'Contesto', p.context.toLocaleString(formatLocale) + ' token' ],
                    [ 'Thread CPU', p.threads || r?.threads || '—' ],
                    ['Modello attuale', m?.name || current.connection.model || '—'],
                    [ 'Rilascia memoria', residencyLabel(p.keepAlive) ]
                ]));
                const c = current.lastCompletion;
                const memory = window.modelMemory, estimate = memory?.estimate, runtime = memory?.runtime;
                if (estimate?.bytes && estimate.id === m?.id && estimate.context === p.context &&
                    estimate.gpuLayers === p.gpuLayers)
                    out.append(detailCard('Memoria modello', 'memory-stick', [
                        ['Origine', tr(estimate.source === 'buffers' ? 'Buffer del motore' :
                                      estimate.source === 'reported' ? 'Allocazione dichiarata' :
                                      estimate.source === 'observed' ? 'Proiezione dal modello caricato' :
                                      estimate.source === 'engine' ? 'Piano del motore' : 'Metadati · indicativa')],
                        ['Contesto', estimate.context.toLocaleString(formatLocale) + ' token'],
                        ['Pesi', estimate.weights ? sizes(estimate.weights) : undefined],
                        ['Cache KV', estimate.cache ? sizes(estimate.cache) : undefined],
                        ['Buffer di calcolo', estimate.compute ? sizes(estimate.compute) : undefined],
                        ['Impronta fisica motore', runtime?.processKnown && runtime.id === m?.id ? sizes(runtime.processBytes) : '—'],
                        ['Allocazione dichiarata', runtime?.allocationBytes ? sizes(runtime.allocationBytes) : undefined],
                        ['Quota allocata su GPU', runtime?.allocationBytes ? sizes(runtime.gpuAllocationBytes) : undefined]
                    ]));
                if (c?.durationMs) {
                    const seconds = ms => (ms / 1000).toLocaleString(formatLocale, {maximumFractionDigits : 2}) + ' s';
                    out.append(detailCard('Ultima risposta', 'gauge', [
                        ['Modello', current.lastModel || '—'],
                        ['Motore', current.lastRuntime || undefined],
                        ['Tempo risposta', seconds(c.durationMs)],
                        ['Primo token', c.firstOutputMs > 0 ? seconds(c.firstOutputMs) : undefined],
                        ['Tempo ragionamento', c.thinkingMs > 0 ? seconds(c.thinkingMs) : undefined],
                        ['Ragionamento richiesto', c.requestedThinking ? thinkingLabel(c.requestedThinking) : undefined],
                        ['Token di input', c.inputTokens > 0 ? c.inputTokens.toLocaleString(formatLocale) : undefined],
                        ['Input dalla cache', c.cachedInputTokens > 0 ? c.cachedInputTokens.toLocaleString(formatLocale) : undefined],
                        ['Token di output', c.outputTokens > 0 ? c.outputTokens.toLocaleString(formatLocale) : undefined],
                        ['Velocità risposta', c.outputTokens > 0 && c.evalMs > 0 ?
                            (c.outputTokens / (c.evalMs / 1000)).toLocaleString(formatLocale, {maximumFractionDigits : 1}) + ' token/s' : undefined],
                        ['Termine', c.stopReason ? tr({stop:'Completata', length:'Limite di output raggiunto.',
                            cancelled:'Interrotta', error:'Errore', tool_calls:'Strumenti richiesti',
                            content_filter:'Limitata dal servizio'}[c.stopReason] || 'Termine non specificato') : undefined]
                    ]));
                }
                if (r)
                    out.append(detailCard('Risorse', 'monitor', [
                        [ 'CPU', r.cores + ' core' ], [ 'Memoria disponibile', gib(r.available) ],
                        [ 'Budget GPU', r.gpu ? gib(r.gpuBudget) : '—' ], [ 'Spazio libero', gib(r.disk) ]
                    ]));
            }
        } else {
            const rows = route === 'knowledge' ? detailTab === 'documents' ?
                [['Documenti',tr('Predisposto')],['Formato','Markdown'],['Archiviazione',tr('Locale')]] :
                detailTab === 'index' ? [['Documenti indicizzati','0'],['Chunk','0'],['RAG',tr('Non attivo')]] :
                [['Pagine web salvate',tr('Predisposto')],['Web',tr('Non attivo')]] : route === 'projects' ?
                detailTab === 'scope' ? [['Modalità progetto',tr('Predisposto')],['Archiviazione',tr('Locale')]] :
                detailTab === 'memory' ? [['Chat',tr('Predisposto')],['Materiali',tr('Predisposto')]] :
                [['Strumenti',tr('Non attivo')],['Shell/Python',tr('Non attivo')]] :
                [['Strumenti',tr('Predisposto')],['MCP',tr('Predisposto')],['Shell/Python',tr('Non attivo')]];
            out.append(detailCard(tr(definition.find(x => x[0] === detailTab)[1]),
                                  route === 'knowledge'  ? 'library'
                                  : route === 'projects' ? 'folder-open'
                                                         : 'blocks',
                                  rows));
        }
    }
    if (route === 'models' && detailKind === 'activity' && item) {
        const live = {
            updated : formatTimestamp(item.updated),
            received : sizes(item.received), total : item.total ? sizes(item.total) : '—',
            speed : transferEstimate(item) || '—'
        };
        for (const [key, value] of Object.entries(live)) {
            const field = $('job-detail-' + key);
            if (field)
                field.textContent = value;
        }
    }
};
