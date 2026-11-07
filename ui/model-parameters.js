'use strict';
// A transient draft belongs to this dialog; only Save changes the C-owned preferences.
const modelPresets = [
    [ 'recommended', 'Consigliato', 'thumbs-up' ],
    [ 'light', 'Memoria ridotta', 'leaf' ], [ 'balanced', 'Bilanciato', 'scale' ],
    [ 'long', 'Contesto esteso', 'book-open' ], [ 'max', 'Contesto massimo', 'gauge' ]
];
function presetValues(id) {
    return {...libraryState?.profiles?.[id], profile : id};
}
function retentionSupported() {
    const model = activeLibraryModel();
    return model ? !!libraryState?.engines.find(e => e.kind === model.kind)?.idleSleep
                 : !!current?.connection.ollama;
}
function residencyLabel(value) {
    if (value === '0') return tr('A fine risposta');
    if (value === '-1') return tr('Mai');
    const minutes = parseInt(value, 10);
    return quantity(minutes % 60 ? 'idleMinutes' : 'idleHours', minutes % 60 ? minutes : minutes / 60);
}
function residencyControl(onChange) {
    const element = make('select');
    element.className = 'residency-control';
    element.setAttribute('aria-label', tr('Rilascia memoria'));
    const durations = [1, 2, 5, 10, 15, 30, 60, 120, 240];
    element.onchange = () => onChange(element.value);
    return {element, set(value, enabled) {
        element.setAttribute('aria-label', tr('Rilascia memoria'));
        const values = ['0', ...durations.map(n => n + 'm'), '-1'];
        if (!values.includes(value))
            values.splice(values.length - 1, 0, value);
        const signature = JSON.stringify([language, values]);
        if (element.dataset.options !== signature) {
            element.dataset.options = signature;
            element.replaceChildren(...values.map(next => new Option(residencyLabel(next), next)));
        }
        element.value = value;
        element.disabled = !enabled;
        element.dataset.tip = enabled ? '' : tr('Gestito dal motore selezionato');
    }};
}
let parameterDraft = null, parameterSavePending=false, estimateTimer = 0, estimateSent = 0, estimateKey = '',
    parameterModelId = 0, parameterTargetId = 0, parameterSwitching = false, parameterEstimate = null,
    refreshParameterMetadata = null;
function requestMemoryEstimate(model) {
    clearTimeout(estimateTimer);
    const {context, gpuLayers, memoryPercent, threads} = parameterDraft,
          key = [model.id, context, gpuLayers, memoryPercent, threads].join(':');
    if (key === estimateKey)
        return;
    estimateTimer = setTimeout(() => {
        estimateSent = performance.now();
        estimateKey = key;
        libraryCommand('estimate', {id : model.id, context, gpuLayers, memoryPercent, threads});
    }, Math.max(0, 80 - (performance.now() - estimateSent)));
}
$('model-parameters').addEventListener('close', () => {
    clearTimeout(estimateTimer);
    estimateKey = '';
    parameterDraft = null;parameterSavePending=false;
    refreshParameterMetadata = null;
    libraryCommand('cancelEstimate');
    const model = activeLibraryModel();
    if (model && current)
        libraryCommand('estimate', {id : model.id, context : current.preferences.context,
                                     gpuLayers : current.preferences.gpuLayers});
});
function modelParameters() {
    if (!current)
        return;
    if (!activeLibraryModel()) send({command:'connectionRefresh'});
    const m = activeLibraryModel(), d = $('model-parameters');
    let limit = Math.max(1024, Math.min(262144, m?.context || current.connection.context || 262144));
    clearTimeout(estimateTimer);
    parameterDraft = {...current.preferences};parameterSavePending=false;
    estimateKey = '';
    parameterSwitching = false;
    parameterModelId = m?.id || 0;
    if (parameterEstimate?.id !== parameterModelId)
        parameterEstimate = null;
    parameterTargetId = parameterModelId;
    const initial = {...parameterDraft};
    $('model-parameters-heading').textContent = tr('Parametri del modello');
    $('close-model-parameters').onclick = () => closeDialog(d);
    const form = $('model-parameters-form');
    form.replaceChildren();
    const select = make('select');
    select.setAttribute('aria-label', tr('Modello'));
    for (const model of m ? libraryState.models : []) {
        const option = make('option', modelLabel(model));
        option.value = model.id;
        select.append(option);
    }
    if (!m) {
        const configured = make('option', connectionLabel() || tr('Nessun modello'));
        configured.value = '';
        select.prepend(configured);
    }
    select.value = m?.id || '';
    select.disabled = !!(current.active || current.improving || libraryState?.busy || !m);
    select.onchange = () => {
        clearTimeout(estimateTimer);
        estimateKey = '';
        parameterSwitching = true;
        parameterTargetId = Number(select.value);
        for (const control of form.querySelectorAll('input,button,select'))
            control.disabled = true;
        const next = libraryState.models.find(model => model.id === parameterTargetId);
        summary.querySelector('dl').replaceWith(metadata([
            ['Modello', modelLabel(next) || '—'], ['Stato', tr('Preparazione')]
        ]));
        window.updateModelMemory();
        libraryCommand('cancelEstimate');
        libraryCommand('select', {id : parameterTargetId});
    };
    const summary = make('div', '', 'model-parameter-summary');
    summary.append(
        select, metadata([
            [ m ? 'Motore' : 'Protocollo', m ? (m.kind === 1 ? 'llama.cpp' : 'Ollama') : current.connection.ollama ? 'Ollama' : tr('OpenAI compatibile') ],
            [ 'Stato', runtimeDescription(), 'model-runtime-state' ]
        ]));
    const facts = make('div', '', 'model-parameter-facts');
    if (m?.metadata?.expertCount) {
        const experts = m.metadata.expertsUsed;
        facts.append(make('span', 'MoE · ' + (experts ? experts + ' / ' : '') +
            m.metadata.expertCount + ' ' + tr('expert'), 'badge'));
    }
    if (facts.childElementCount)
        summary.append(facts);
    const presets = make('div', '', 'model-param-presets');
    for (const [id, label, glyph] of modelPresets) {
        const b = button(label, () => {
            Object.assign(parameterDraft, presetValues(id));
            syncFields();
        });
        b.type = 'button';
        b.dataset.preset = id;
        b.replaceChildren(icon(glyph), make('span', tr(label)));
        b.disabled = id === 'max' && !(m?.context || current.connection.context);
        if (b.disabled) b.dataset.tip = tr('Contesto massimo non rilevato per questo modello');
        presets.append(b);
    }
    const memory = make('section', '', 'model-memory');
    memory.id = 'model-memory';
    const fields = make('div', '', 'model-parameter-fields'), sliders = new Map(),
          limits = make('div', '', 'model-parameter-column'), execution = make('div', '', 'model-parameter-column');
    fields.append(limits, execution);
    function slider(key, label, min, max, step, parent = limits, format = value => value.toLocaleString(formatLocale)) {
        const row = make('div', '', 'model-slider'), head = make('div', '', 'row'),
              title = make('label', tr(label)), range = make('input'), value = make('input');
        range.type = 'range';
        range.name = key;
        range.id = 'model-param-' + key;
        range.min = min;
        range.max = max;
        range.step = step;
        value.type = 'number';
        value.inputMode = step < 1 ? 'decimal' : 'numeric';
        value.autocomplete = 'off';
        value.min = min;
        value.max = max;
        value.step = key === 'context' ? 1 : step;
        value.setAttribute('aria-label', tr(label));
        title.htmlFor = range.id;
        if (key === 'memoryPercent') {
            const field = make('span', '', 'model-number');
            field.append(value, make('span', '%'));
            head.append(title, field);
        } else
            head.append(title, value);
        row.append(head, range);
        parent.append(row);
        const entry = {range, value, format};
        sliders.set(key, entry);
        function update(input) {
            const number = input.valueAsNumber;
            if (!Number.isFinite(number))
                return;
            parameterDraft[key] = Math.min(Number(range.max), Math.max(min, number));
            parameterDraft.profile = 'custom';
            syncFields();
        }
        range.oninput = () => update(range);
        value.onchange = () => update(value);
    }
    slider('context', 'Contesto', 1024, limit, 512);
    slider('input', 'Input massimo', 1, limit, 1);
    slider('output', 'Output massimo', 64, Math.min(32768, limit - 513), 1);
    const sampling = make('div', '', 'model-sampling');
    (m ? limits : execution).append(sampling);
    slider('temperature', 'Temperatura', 0, 2, .1, sampling);
    slider('topP', 'Top P', .05, 1, .05, sampling);
    slider('memoryPercent', 'Budget memoria', 10, 100, 5, execution, value => value + '%');
    sliders.get('memoryPercent').range.closest('.model-slider').hidden = !m;
    memory.hidden = !m;
    const gpu = make('div', '', 'model-slider'), modes = make('div', '', 'visual-choices');
    gpu.append(make('label', tr('Uso GPU')), modes);
    gpu.hidden = !m;
    const cpuMaximum = m?.cpuMaxContext || 0;
    for (const [value, label] of [[-1, 'GPU'], [0, 'Solo CPU']]) {
        const b = button(label, () => {
            parameterDraft.gpuLayers = value;
            if (value === 0)
                parameterDraft.threads = 0;
            parameterDraft.requireGpu = value !== 0;
            parameterDraft.profile = 'custom';
            syncFields();
        });
        b.type = 'button';
        b.dataset.mode = value;
        b.prepend(icon(value === 0 ? 'cpu' : 'monitor'));
        b.dataset.tip = value === 0 ? tr('CPU non verificata con questi parametri') : '';
        modes.append(b);
    }
    execution.append(gpu);
    const keep = make('div', '', 'model-slider model-retention'),
          retention = residencyControl(keepAlive => {
              parameterDraft.keepAlive = keepAlive;
              parameterDraft.profile = 'custom';
              syncFields();
          });
    keep.append(make('label', tr('Rilascia memoria')), retention.element);
    keep.hidden = !retentionSupported();
    execution.append(keep);
    const foot = make('div', '', 'dialog-footer'), error = make('p'),
          reset = button('Valori predefiniti', () => {
              parameterDraft = {...initial, ...libraryState.modelDefaults};
              syncFields();
          }), save = button('Salva', () => {});
    error.id = 'model-parameters-error';
    error.dataset.feedback = '';
    error.setAttribute('role', 'alert');
    reset.type = 'button';
    save.type = 'submit';
    save.id = 'apply-model-parameters';
    save.className = 'primary';
    foot.append(error, reset, save);
    const body = make('div', '', 'model-parameter-body');
    body.append(summary, presets, memory, fields);
    form.append(body, foot);
    function syncFields() {
        const p = parameterDraft;
        p.context = Math.min(limit, Math.max(1024, p.context));
        const inputMinimum = current.minimumInputBytes || 64,
              outputMax = Math.min(32768, p.context - Math.max(513, inputMinimum + 256));
        p.output = Math.max(64, Math.min(outputMax, p.output));
        const inputMax = p.context - p.output - 256;
        p.input = Math.max(Math.min(inputMinimum, inputMax), Math.min(p.input || inputMax, inputMax));
        sliders.get('output').range.max = sliders.get('output').value.max = Math.max(64, outputMax);
        sliders.get('input').range.min = sliders.get('input').value.min = Math.min(inputMinimum, inputMax);
        sliders.get('input').range.max = sliders.get('input').value.max = inputMax;
        for (const [key, entry] of sliders) {
            entry.range.value = entry.value.value = p[key];
            entry.range.setAttribute('aria-valuetext', entry.format(Number(entry.range.value)));
        }
        const cpuAllowed = cpuMaximum >= p.context;
        if (p.gpuLayers === 0 && !cpuAllowed)
            p.gpuLayers = -1;
        if (p.gpuLayers !== 0)
            p.gpuLayers = -1;
        p.requireGpu = p.gpuLayers !== 0;
        for (const b of modes.children) {
            b.disabled = Number(b.dataset.mode) === 0 && !cpuAllowed;
            b.setAttribute('aria-pressed', String(Number(b.dataset.mode) === p.gpuLayers));
        }
        const selected = p.profile;
        for (const b of presets.children)
            b.setAttribute('aria-pressed', String(b.dataset.preset === selected));
        retention.set(p.keepAlive, retentionSupported());
        setFeedback(error,outputMax < 64 ? 'Aumenta il contesto o riduci le istruzioni della chat.' : '', 'warning');
        save.disabled = outputMax < 64;
        save.textContent = tr('Salva');
        if (m)
            requestMemoryEstimate(m);
        window.updateModelMemory();
    }
    form.onsubmit = event => {
        event.preventDefault();
        if (!form.reportValidity())
            return;
        for (const [key, entry] of sliders)
            if (entry.value.valueAsNumber !== parameterDraft[key]) {
                parameterDraft[key] = entry.value.valueAsNumber;
                parameterDraft.profile = 'custom';
            }
        syncFields();
        if (save.disabled) return;
        save.disabled = true;parameterSavePending=true;
        send({command : 'preferences', value : parameterDraft});
    };
    window.modelParametersSaved = () => {
        if (!d.open || !parameterSavePending)
            return;
        parameterSavePending=false;
        if (window.guideParametersSaved?.()) return;
        save.disabled = false;
        save.replaceChildren(icon('check'), document.createTextNode(tr('Salvato')));
    };
    window.modelParametersFailed=() => {parameterSavePending=false;save.disabled=false;};
    refreshParameterMetadata = () => {
        const next = Math.max(1024,Math.min(262144,current.connection.context || 262144));
        const max = presets.querySelector('[data-preset=max]');
        if (select.options[0].textContent !== connectionLabel()) select.options[0].textContent = connectionLabel();
        if (next === limit && max.disabled === !current.connection.context) return;
        limit = next;
        sliders.get('context').range.max = sliders.get('context').value.max = limit;
        max.disabled = !current.connection.context;
        max.dataset.tip = max.disabled ? tr('Contesto massimo non rilevato per questo modello') : '';
        syncFields();
    };
    syncFields();
    const wasOpen = d.open;
    openDialog(d);
    if (wasOpen)
        select.focus();
}
window.refreshModelParameters = () => {
    const d = $('model-parameters'), m = activeLibraryModel();
    if (!d.open || libraryState?.busy)
        return;
    if (parameterSwitching && libraryState?.error) {
        const error = libraryState.error;
        modelParameters();
        setFeedback($('model-parameters-error'),libraryErrors[error] || explain(error),feedbackKind(error));
        return;
    }
    if (!m) {
        if (parameterModelId) modelParameters();
        else refreshParameterMetadata?.();
        return;
    }
    if ((!parameterSwitching || m.id === parameterTargetId) &&
        (m.id !== parameterModelId || parameterSwitching) &&
        (libraryState.engine === 'failed' ||
         current.ready && current.connection.model === (m.kind === 1 ? m.name : m.file)))
        modelParameters();
    else {
        const selector = d.querySelector('select');
        const disabled = !!(parameterSwitching || current.active || current.improving || !libraryState.models.length);
        if (selector && selector.disabled !== disabled)
            selector.disabled = disabled;
    }
};
window.overtoneMemory = state => {
    window.modelMemory = state;
    window.updateModelMemory();
    window.updateShell?.();
    $('engine-status').textContent = runtimeDescription();
    window.renderDetails?.();
};
window.updateModelMemory = () => {
    const out = $('model-memory'), r = window.lastResources;
    if (!out || !r?.total || !parameterDraft)
        return;
    const status = $('model-runtime-state');
    if (status && !parameterSwitching)
        status.textContent = runtimeDescription();
    if (!out.querySelector('.memory-track')) {
        const head = make('div', '', 'row');
        head.append(make('span', '', 'memory-name'), make('span', '', 'memory-capacity'));
        const track = make('div', '', 'meter-track memory-track');
        track.setAttribute('role', 'img');
        const projection = make('span', '', 'memory-extra');
        projection.append(make('i'));
        track.append(make('span', '', 'memory-system'), make('span', '', 'memory-engine'), projection);
        const legend = make('div', '', 'memory-legend');
        for (const [kind, text, segment] of [['used', 'Occupata', 'system'], ['active', 'Motore', 'engine'],
                                            ['estimate', 'Modello', 'extra']]) {
            const item = make('div', '', 'memory-' + kind + '-value'),
                  label = make('span', '', 'memory-legend-label'),
                  swatch = make('span', '', 'memory-swatch memory-' + segment);
            swatch.setAttribute('aria-hidden', 'true');
            label.append(swatch, document.createTextNode(tr(text)));
            item.append(label, make('strong'));
            legend.append(item);
        }
        out.append(head, track, legend);
    }
    const label = tr(r.unified ? 'Memoria unificata' : 'Memoria');
    out.querySelector('.memory-name').textContent = label;
    out.querySelector('.memory-capacity').textContent = gib(r.total);
    const m = activeLibraryModel(), state = window.modelMemory, runtime = state?.runtime,
          candidate = state?.estimate,
          matches = !parameterSwitching && candidate && m && candidate.id === m.id && candidate.context === parameterDraft.context &&
                    candidate.gpuLayers === parameterDraft.gpuLayers &&
                    candidate.memoryPercent === parameterDraft.memoryPercent && candidate.threads === parameterDraft.threads;
    const preserve = matches && candidate.source === 'metadata' && parameterEstimate?.source !== 'metadata' &&
                     parameterEstimate?.context === candidate.context && parameterEstimate?.gpuLayers === candidate.gpuLayers &&
                     parameterEstimate?.memoryPercent === candidate.memoryPercent && parameterEstimate?.threads === candidate.threads;
    if (matches && candidate.bytes && !preserve)
        parameterEstimate = candidate;
    const estimate = parameterSwitching || parameterEstimate?.id !== m?.id ? null : parameterEstimate,
          pending = !matches || !!candidate?.pending || preserve,
          known = !parameterSwitching && runtime?.processKnown && runtime.id === m?.id,
          process = known ? runtime.processBytes : 0,
          used = Math.min(r.total, Math.max(0, r.used)), engine = Math.min(used, process),
          // Allocation plans are not physical footprints. Credit only measured
          // residency, not a reserved allocation, against additional demand.
          // A shared engine with other models cannot be attributed to this one.
          resident = known && libraryState.engine === 'ready' &&
                     runtime.acceleration !== 'asleep' && !runtime.otherResidentModels ? engine : 0,
          extra = estimate?.bytes ? Math.max(0, estimate.bytes - resident) : 0,
          modelEnd = Math.min(r.total, used + extra),
          amount = bytes => bytes >= 1073741824
              ? (bytes / 1073741824).toLocaleString(formatLocale, {maximumFractionDigits : 2}) + ' GiB'
              : sizes(bytes);
    let offset = 0;
    for (const [kind, bytes] of [['system', used - engine], ['engine', engine]]) {
        out.querySelector('.memory-' + kind).style.transform =
            `translateX(${offset / r.total * 100}%) scaleX(${bytes / r.total})`;
        offset += bytes;
    }
    // Two translated clips retain the hatch spacing at display cadence, without
    // scaling the pattern or interpolating a clip-path on WebKit's main thread.
    const projection = out.querySelector('.memory-extra');
    projection.style.transform = `translateX(${(modelEnd / r.total - 1) * 100}%)`;
    projection.firstElementChild.style.transform = `translateX(${(1 - (modelEnd - used) / r.total) * 100}%)`;
    const usedValue = out.querySelector('.memory-used-value'), activeValue = out.querySelector('.memory-active-value'),
          estimateValue = out.querySelector('.memory-estimate-value');
    usedValue.querySelector('strong').textContent = amount(used);
    activeValue.querySelector('strong').textContent = known ? amount(process) : '—';
    estimateValue.querySelector('strong').textContent = estimate?.bytes ? '≈ ' + amount(estimate.bytes) : '—';
    estimateValue.setAttribute('aria-busy', String(pending));
    const policy = r.total / 100 * parameterDraft.memoryPercent,
          budget = r.pressure >= 2 ? 0 : Math.min(policy, r.budget + process);
    out.classList.toggle('budget-exceeded', !!estimate?.bytes && estimate.bytes > budget);
    const track = out.querySelector('.memory-track');
    track.setAttribute('aria-label', label + ': ' + amount(used) + ' / ' + amount(r.total) + '. ' +
        tr('Motore') + ': ' + (known ? amount(process) : '—') + '. ' +
        tr('Modello') + ': ' + (estimate?.bytes ? '≈ ' + amount(estimate.bytes) : '—'));
};
