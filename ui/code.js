'use strict';
let codeHighlightTimer = 0, codeMetricTimer = 0, codeHighlightMs = 0, codeMetricSignature = '';
let expandedCode = null, codeScanQueued = false;
let codeObserver = null, codeObserverRoot = null, codeObserverDocument = null;
const observedCode = new Set(), visibleCode = new Set();
window.closeCodeView = () => {
    expandedCode?.pre.remove();
    expandedCode = null;
    $('action-dialog').classList.remove('code-viewer');
    $('action-dialog').querySelector('.code-viewer-actions')?.remove();
};
function syncExpandedCode() {
    const view = expandedCode;
    if (!view || !$('action-dialog').open)
        return;
    const block = view.block, source = block.querySelector('code'), selection = view.pre.ownerDocument.getSelection(),
          wrapped = block.dataset.wrap === 'auto' ? !!current?.preferences.codeWrap : block.dataset.wrap === 'true';
    view.pre.dataset.wrap = String(wrapped);
    view.wrap.setAttribute('aria-pressed', String(wrapped));
    if (view.text === block.codeSource && view.highlighted === block.dataset.highlighted)
        return;
    if (selection?.rangeCount && !selection.isCollapsed && selection.getRangeAt(0).intersectsNode(view.pre))
        return;
    view.pre.replaceChildren(source.cloneNode(true));
    view.text = block.codeSource;
    view.highlighted = block.dataset.highlighted;
}
function openCodeView(block) {
    const pre = make('pre', '', 'expanded-code'), actions = make('div', '', 'code-viewer-actions'),
          wrap = actionButton('A capo nel codice','wrap-text', () => {
              block.querySelector('.code-actions button').click();
              syncExpandedCode();
          }), copy = copyButton(() => block.codeSource);
    copy.copyLabel = tr('Copia codice');
    copy.setAttribute('aria-label',copy.copyLabel);
    copy.dataset.tip = copy.copyLabel;
    actions.append(wrap,copy);
    const dialog = actionDialog(tr('Codice') + ' · ' + (block.codeLanguage || 'text'),pre);
    dialog.classList.add('code-viewer');
    dialog.querySelector('.dialog-heading').insertBefore(actions,$('close-action-dialog'));
    expandedCode = {block,pre,wrap};
    highlightVisibleCode();
    syncExpandedCode();
}
$('action-dialog').addEventListener('close',window.closeCodeView);
onUIEvent('selectionchange',syncExpandedCode);
const codeKeywords = new Set(
    ('alignas alignof and as asm async await auto bool break case catch char class ' +
     'const constexpr continue def default delete do double elif else enum except export extern false final ' +
     'finally float for from func function goto if implements import in inline int interface is lambda let ' +
     'long match namespace new nil noexcept not null nullptr object operator or override pass private protected ' +
     'public raise readonly register return self short signed sizeof static struct super switch template this ' +
     'throw throws true try typedef typename typeof union unsigned using var virtual void volatile while with ' +
     'yield select insert update delete create table where join inner left right order group by having limit ' +
     'values into set distinct as on primary key references check not exists begin end commit rollback')
        .split(/\s+/));
const codeLanguages = new Set(
    ('c cpp c++ cc cxx h hpp objc objective-c java javascript js typescript ts jsx tsx ' +
     'python py ruby rb rust rs swift go kotlin kt csharp cs c# php lua shell sh bash zsh powershell ps1 ' +
     'sql json jsonc yaml yml toml ini css scss html xml dockerfile diff patch')
        .split(/\s+/));
function lexerLanguage(language) {
    const name = language.toLowerCase();
    if (/^c(?:89|90|99|11|17|23)$/.test(name)) return 'c';
    if (/^(?:c\+\+|cpp)(?:98|03|11|14|17|20|23|26)$/.test(name)) return 'cpp';
    return { 'objectivec':'objc', 'objective-c++':'cpp', 'c++':'cpp', 'c#':'cs',
             'javascriptreact':'jsx', 'typescriptreact':'tsx', 'console':'shell' }[name] || name;
}
const cKeywords = new Set(('alignas alignof asm auto bool break case char const constexpr continue ' +
    'default do double else enum extern false float for goto if inline int long nullptr register ' +
    'restrict return short signed sizeof static static_assert struct switch thread_local true typedef ' +
    'typeof typeof_unqual union unsigned void volatile while _Alignas _Alignof _Atomic _Bool _Complex ' +
    '_Generic _Imaginary _Noreturn _Static_assert _Thread_local').split(/\s+/));
const cppKeywords = new Set([...cKeywords, ...('and and_eq bitand bitor catch class concept consteval ' +
    'constinit const_cast co_await co_return co_yield decltype delete dynamic_cast explicit export ' +
    'friend mutable namespace new noexcept not not_eq operator or or_eq private protected public ' +
    'reinterpret_cast requires static_cast template this throw try typeid typename using virtual ' +
    'wchar_t xor xor_eq').split(/\s+/)]);
function appendCodeText(target, text, kind = '') {
    return appendTextRuns(target, text, kind ? 'code-text syntax-' + kind : 'code-text');
}
function codeBlock(source, language, closed) {
    language = language.split(/\s/)[0].slice(0, 64);
    const block = document.createElement('section');
    block.className = 'code-block';
    const head = document.createElement('header'), name = document.createElement('span'),
          actions = document.createElement('div'), pre = document.createElement('pre'),
          code = document.createElement('code');
    name.textContent = language || 'text';
    actions.className = 'code-actions';
    const wrap = actionButton('A capo nel codice', 'wrap-text', () => {
        const wrapped = block.dataset.wrap === 'auto' ? !!current?.preferences.codeWrap
                                                      : block.dataset.wrap === 'true';
        const value = !wrapped;
        block.dataset.wrap = String(value);
        wrap.setAttribute('aria-pressed', String(value));
        window.transcriptView?.settle();
    });
    const copy = copyButton(() => code.textContent);
    copy.copyLabel = tr('Copia codice');
    copy.setAttribute('aria-label', copy.copyLabel);
    copy.dataset.tip = copy.copyLabel;
    const expand = actionButton('Espandi codice','expand', () => openCodeView(block));
    actions.append(wrap,expand,copy);
    head.append(name, actions);
    pre.append(code);
    block.append(head, pre);
    block.codeLanguage = language;
    block.codeSource = source;
    block.dataset.closed = String(closed);
    block.dataset.wrap = 'auto';
    block.dataset.plain = String(!language);
    wrap.setAttribute('aria-pressed', String(!!current?.preferences.codeWrap));
    const lines = appendCodeText(code, source);
    block.style.setProperty('--code-lines', Math.max(0, lines));
    block.dataset.fixedLines = String(lines >= 0);
    return block;
}
function updateCodeBlock(old, next) {
    // During streaming the fence arrives before its language ("```", then "c").
    // Keep DOM, lexer identity and source in sync instead of generic child reconciliation.
    if (old.codeLanguage !== next.codeLanguage) {
        old.codeLanguage = next.codeLanguage;
        old.querySelector('header>span').textContent = next.codeLanguage || 'text';
        old.dataset.plain = String(!next.codeLanguage);
        old.codePrefix = null;
        delete old.dataset.highlighted;
    }
    if (old.codeSource !== next.codeSource) {
        const code = old.querySelector('code'), text = next.codeSource;
        if (old.dataset.closed === 'false' && text.startsWith(old.codeSource))
            appendCodeText(code, text.slice(old.codeSource.length));
        else {
            code.replaceChildren();
            appendCodeText(code, text);
            old.codePrefix = null;
        }
        old.codeSource = text;
        old.style.setProperty('--code-lines', next.style.getPropertyValue('--code-lines'));
        old.dataset.fixedLines = next.dataset.fixedLines;
        delete old.dataset.highlighted;
    }
    old.dataset.closed = next.dataset.closed;
}
window.flushCodePreference = () => {
    for (const block of $('transcript').querySelectorAll('.code-block[data-wrap=auto]'))
        block.querySelector('.code-actions button')
            .setAttribute('aria-pressed',
                          String(!!current?.preferences.codeWrap));
};
function highlightVisibleCode() {
    if (codeScanQueued) return;
    codeScanQueued = true;
    // New messages are assembled off-document. Discover blocks after reconciliation
    // has attached them, once per batch rather than once per message/stream fragment.
    queueMicrotask(() => {
        codeScanQueued = false;
        const root = $('transcript');
        if (codeObserverRoot !== root || codeObserverDocument !== root.ownerDocument) {
            codeObserver?.disconnect();
            observedCode.clear();
            visibleCode.clear();
            codeObserverRoot = root;
            codeObserverDocument = root.ownerDocument;
            codeObserver = new root.ownerDocument.defaultView.IntersectionObserver(entries => {
                for (const entry of entries)
                    if (entry.isIntersecting) visibleCode.add(entry.target);
                    else visibleCode.delete(entry.target);
                scheduleCodeHighlight();
            }, {root,rootMargin:'150px 0px'});
        }
        const blocks = new Set(root.querySelectorAll('.code-block'));
        for (const block of observedCode)
            if (!blocks.has(block)) {
                codeObserver.unobserve(block);
                observedCode.delete(block);
                visibleCode.delete(block);
            }
        for (const block of blocks)
            if (!observedCode.has(block)) {
                observedCode.add(block);
                codeObserver.observe(block);
            }
        scheduleCodeHighlight();
    });
}
function scheduleCodeHighlight() {
    if (codeHighlightTimer)
        return;
    // Native state updates may arrive while WebKit has suspended animation frames.
    // Batch after DOM reconciliation, so first-paint colour does not depend on focus.
    codeHighlightTimer = 1;
    queueMicrotask(() => {
        codeHighlightTimer = 0;
        const started = performance.now();
        if ($('chat-workspace').inert || !motionDocumentVisible($('transcript').ownerDocument))
            return;
        const visible = [];
        const candidates = expandedCode ? new Set([...visibleCode,expandedCode.block]) : visibleCode;
        for (const block of candidates) {
            if (!block.isConnected || block.dataset.highlighted)
                continue;
            const code = block.querySelector('code'), selection = code.ownerDocument.getSelection();
            if (selection?.rangeCount && !selection.isCollapsed && selection.getRangeAt(0).intersectsNode(code))
                continue; // Selectionchange retries these blocks; never spin while the user is reading.
            if (visible.length === 2) {
                setTimeout(scheduleCodeHighlight, 0);
                break;
            }
            visible.push({block,code});
        }
        // Read visibility before changing any code. A second block must not force
        // layout of the first one's newly coloured text in the same batch.
        for (const {block,code} of visible) {
            block.dataset.highlighted = 'true';
            const text = block.codeSource, language = lexerLanguage(block.codeLanguage),
                  cFamily = ['c','h','cpp','c++','cc','cxx','hpp','objc','objective-c'].includes(language),
                  markup = ['html','xml'].includes(language),
                  hashComment = ['python','py','ruby','rb','shell','sh','bash','zsh','powershell','ps1',
                                 'yaml','yml','toml','ini','dockerfile'].includes(language),
                  slashComment = !markup && !hashComment && !['sql','lua','diff','patch'].includes(language),
                  keywords = cFamily ? (['c','h','objc','objective-c'].includes(language) ? cKeywords : cppKeywords)
                                     : codeKeywords;
            if (text.length > 131072 || !codeLanguages.has(language))
                continue;
            // A bounded lexical aid, not a compiler or grammar validator. Always create text nodes.
            const fragment = document.createDocumentFragment();
            const prefix = block.codePrefix;
            let start = prefix?.end || 0, offset = start, count = prefix?.count || 0,
                stable = start, stableCount = count;
            while (offset < text.length && count < 8192) {
                let end = offset, kind = '';
                const first = text[offset], pair = text.slice(offset, offset + 2);
                // Only a newline outside a lexical token is a safe restart point.
                // An unfinished multiline string/comment must be read again from its start.
                if (first === '\n') { stable = offset + 1; stableCount = count; }
                if (pair === '//' && slashComment || first === '#' && hashComment ||
                    pair === '--' && ['sql','lua'].includes(language)) {
                    end = text.indexOf('\n', offset);
                    if (end < 0)
                        end = text.length;
                    kind = 'comment';
                } else if (first === '#' && cFamily) {
                    end = offset + 1;
                    while (end < text.length && /[A-Za-z_]/.test(text[end])) end++;
                    kind = 'directive';
                } else if (pair === '/*' && slashComment) {
                    end = text.indexOf('*/', offset + 2);
                    end = end < 0 ? text.length : end + 2;
                    kind = 'comment';
                } else if (markup && text.startsWith('<!--', offset)) {
                    end = text.indexOf('-->', offset + 4);
                    end = end < 0 ? text.length : end + 3;
                    kind = 'comment';
                } else if (markup && first === '<' && /^<\/?[A-Za-z!]/.test(text.slice(offset, offset + 3))) {
                    end = offset + 1;
                    let quote = '';
                    while (end < text.length) {
                        const char = text[end++];
                        if (quote) { if (char === quote) quote = ''; }
                        else if ('"\''.includes(char)) quote = char;
                        else if (char === '>') break;
                    }
                    kind = 'directive';
                } else if (first === '<' && cFamily && /#\s*include\s*$/.test(text.slice(Math.max(0, text.lastIndexOf('\n', offset)), offset))) {
                    const close = text.indexOf('>', offset + 1), newline = text.indexOf('\n', offset + 1);
                    end = close >= 0 && (newline < 0 || close < newline) ? close + 1 : offset + 1;
                    kind = end > offset + 1 ? 'string' : '';
                } else if ('"\'`'.includes(first)) {
                    end = offset + 1;
                    while (end < text.length) {
                        if (text[end] === '\\') {
                            end = Math.min(text.length, end + 2);
                            continue;
                        }
                        if (text[end++] === first)
                            break;
                    }
                    kind = ['json','jsonc'].includes(language) && /^\s*:/.test(text.slice(end)) ? 'property' : 'string';
                } else if (/[A-Za-z_0-9]/.test(first)) {
                    end = offset + 1;
                    while (end < text.length && /[A-Za-z_0-9.]/.test(text[end]))
                        end++;
                    kind = /[0-9]/.test(first)                                       ? 'number'
                           : keywords.has(cFamily ? text.slice(offset, end) : text.slice(offset, end).toLowerCase()) ? 'keyword'
                           : ['yaml','yml','json','jsonc','css','scss'].includes(language) && /^\s*:/.test(text.slice(end)) ? 'property'
                           : /^\s*\(/.test(text.slice(end)) ? 'function'
                           : /^[A-Z_][A-Z_0-9]+$/.test(text.slice(offset,end)) ? 'constant' : '';
                }
                if (kind) {
                    appendCodeText(fragment, text.slice(start, offset));
                    appendCodeText(fragment, text.slice(offset, end), kind);
                    count++;
                    start = end;
                }
                offset = Math.max(offset + 1, end);
            }
            appendCodeText(fragment, text.slice(start));
            let end = prefix?.end || 0, nextPrefix = prefix;
            for (const node of fragment.childNodes) {
                end += node.textContent.length;
                if (end === stable) { nextPrefix = {end, node, count:stableCount}; break; }
                if (end > stable) break;
            }
            if (prefix) {
                while (prefix.node.nextSibling) prefix.node.nextSibling.remove();
                code.append(fragment);
            } else code.replaceChildren(fragment);
            block.codePrefix = nextPrefix;
        }
        codeHighlightMs = Math.max(codeHighlightMs, performance.now() - started);
        if (!codeMetricTimer) codeMetricTimer = setTimeout(reportCodeMetrics, 200);
        syncExpandedCode();
        recordMotionWork('highlight',started);
    });
}
function reportCodeMetrics() {
    codeMetricTimer = 0;
    const blocks = [...$('transcript').querySelectorAll('.code-block')], metrics = {
        blocks: blocks.length,
        coloured: blocks.filter(b => b.querySelector('[class*="syntax-"]')).length,
        pending: blocks.filter(b => !b.dataset.highlighted).length,
        known: blocks.filter(b => codeLanguages.has(lexerLanguage(b.codeLanguage))).length,
        chars: Math.min(100000, blocks.reduce((n,b) => n + b.codeSource.length, 0)),
        mismatched: blocks.filter(b => b.codeSource !== b.querySelector('code').textContent).length,
        highlightMs: Math.round(codeHighlightMs * 100) / 100,
        detailsVisible: Number($('details-panel').classList.contains('open'))
    }, signature = JSON.stringify([metrics.blocks, metrics.coloured, metrics.pending, metrics.detailsVisible,
                                  metrics.known, metrics.chars, metrics.mismatched]);
    codeHighlightMs = 0;
    if (signature !== codeMetricSignature) {
        codeMetricSignature = signature;
        send({command:'renderMetrics', value:metrics});
    }
}
onUIEvent('selectionchange',scheduleCodeHighlight);
