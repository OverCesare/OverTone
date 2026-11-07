'use strict';
// A deliberately small text renderer. No raw HTML, remote images or executable URLs.
// Common mathematical notation uses WebKit's MathML layout. Unsupported TeX stays
// literal: a formatting convenience must never silently change an expression.
function mathExpression(source, display = false) {
    if (!source.trim() || source.length > 2048)
        return null;
    let at = 0, nodes = 0;
    const symbols = {times:'×', cdot:'·', div:'÷', pm:'±', le:'≤', leq:'≤', ge:'≥', geq:'≥',
        ne:'≠', neq:'≠', approx:'≈', infty:'∞', pi:'π', alpha:'α', beta:'β', gamma:'γ',
        delta:'δ', theta:'θ', lambda:'λ', mu:'μ', sigma:'σ', omega:'ω', Delta:'Δ'};
    function make(name, ...children) {
        if (++nodes > 256)
            throw new Error('math limit');
        const node = document.createElementNS('http://www.w3.org/1998/Math/MathML', name);
        node.append(...children);
        return node;
    }
    function space() { while (/\s/.test(source[at] || '') && at < source.length) at++; }
    function atom(depth, single = false) {
        if (depth > 12)
            throw new Error('math depth');
        space();
        const ch = source[at++];
        if (!ch || ch === '}' || ch === '^' || ch === '_')
            throw new Error('math syntax');
        if (ch === '{')
            return row(depth + 1, true);
        if (ch === '\\') {
            const command = source.slice(at).match(/^[a-zA-Z]+|^[ ,;!{}%_]/)?.[0];
            if (!command)
                throw new Error('math command');
            at += command.length;
            if (command === 'left' || command === 'right')
                return atom(depth + 1);
            if (command === 'frac')
                return make('mfrac', atom(depth + 1, true), atom(depth + 1, true));
            if (command === 'sqrt') {
                space();
                if (source[at] === '[')
                    throw new Error('unsupported root index');
                return make('msqrt', atom(depth + 1, true));
            }
            if (command === 'text' || command === 'mathrm') {
                const text = source.slice(at).match(/^\s*\{([^{}]*)\}/);
                if (!text)
                    throw new Error('math text');
                at += text[0].length;
                return make('mtext', text[1].replace(/ /g, '\u00a0'));
            }
            if (symbols[command])
                return make(/[a-zA-Z]/.test(command) && !['times','cdot','div','pm','le','leq',
                    'ge','geq','ne','neq','approx'].includes(command) ? 'mi' : 'mo', symbols[command]);
            if (/^[ ,;!]$/.test(command))
                return make('mtext', command === '!' ? '' : '\u2009');
            if (/^[{}%_]$/.test(command))
                return make('mo', command);
            throw new Error('unsupported math');
        }
        const number = !single && source.slice(at - 1).match(/^(?:[0-9]+(?:\.[0-9]+)?|\.[0-9]+)/);
        if (number) {
            at += number[0].length - 1;
            return make('mn', number[0]);
        }
        return make(/[0-9.]/.test(ch) ? 'mn' : /[\p{L}]/u.test(ch) ? 'mi' : 'mo', ch);
    }
    function row(depth, grouped = false) {
        const children = [];
        while (at < source.length) {
            space();
            if (at === source.length)
                break;
            if (source[at] === '}') {
                if (!grouped)
                    throw new Error('unmatched math');
                at++;
                return make('mrow', ...children);
            }
            let base = atom(depth), sub = null, sup = null;
            space();
            while (source[at] === '^' || source[at] === '_') {
                const script = source[at++];
                if (script === '^') {
                    if (sup) throw new Error('duplicate math script');
                    sup = atom(depth + 1, true);
                } else {
                    if (sub) throw new Error('duplicate math script');
                    sub = atom(depth + 1, true);
                }
                space();
            }
            if (sub || sup)
                base = sub && sup ? make('msubsup', base, sub, sup) : make(sub ? 'msub' : 'msup', base, sub || sup);
            children.push(base);
        }
        if (grouped)
            throw new Error('unclosed math');
        return make('mrow', ...children);
    }
    try {
        const math = make('math', row(0));
        if (display) math.setAttribute('display', 'block');
        return math;
    } catch {
        return null;
    }
}
function textSpan(text) {
    const span = document.createElement('span');
    span.className = text.includes('\n') ? 'selectable-lines' : 'selectable-text';
    if (text.includes('\n')) appendTextRuns(span, text);
    else span.textContent = text;
    return span;
}
function appendTextRuns(target, text, className = 'selectable-text') {
    // Keep literal newlines, but paint only text runs: WebKit otherwise fills
    // the unused width after a preformatted newline when selecting across it.
    const lines = text.split('\n');
    if (typeof Highlight === 'function' && CSS.highlights) {
        // Range highlights paint glyph runs without selection-gap backgrounds.
        // A line wrapper is unnecessary here, especially for unseen code fences.
        if (text) {
            const span = document.createElement('span');
            span.className = className;
            span.textContent = text;
            target.append(span);
        }
    } else lines.forEach((line, index) => {
        if (index) target.append(document.createTextNode('\n'));
        if (line) {
            const span = document.createElement('span');
            span.className = className;
            span.textContent = line;
            target.append(span);
        }
    });
    // Exotic segment separators keep ordinary browser layout, not a guessed line count.
    return /[\r\v\f\u0085\u2028\u2029]/.test(text) ? -1 : text ? lines.length - Number(text.endsWith('\n')) : 0;
}
function inlineText(target, text, depth = 0, budget = {
    inline : 2048
}) {
    // Select the inline content, not the block's padding, margins or empty width.
    // One wrapper per block keeps streaming reconciliation and text shaping intact.
    if (!depth) {
        const content = textSpan('');
        target.append(content);
        target = content;
    }
    if (depth > 4 || text.length > 8192 || budget.inline <= 0) {
        target.append(document.createTextNode(text));
        return;
    }
    const tokens =
        /`([^`\n]+)`|(?<![\p{L}\p{N}\\])\*\*(?!\s)([^\n]+?)(?<!\s)\*\*(?![\p{L}\p{N}])|(?<![\p{L}\p{N}\\])__(?!\s)([^\n]+?)(?<!\s)__(?![\p{L}\p{N}])|~~([^\n]+?)~~|(?<![\p{L}\p{N}\\*])\*(?![\s*])([^*\n]+?)(?<!\s)\*(?![\p{L}\p{N}*])|\[([^\]\n]+)\]\((https?:\/\/[^\s)]+)\)|\\\(([\s\S]*?)\\\)/gu;
    let start = 0, match;
    while ((match = tokens.exec(text))) {
        if (budget.inline --<= 0)
            break;
        target.append(document.createTextNode(text.slice(start, match.index)));
        if (match[8] !== undefined) {
            target.append(mathExpression(match[8]) || document.createTextNode(match[0]));
            start = tokens.lastIndex;
            continue;
        }
        const node = document.createElement(match[1]               ? 'code'
                                            : match[2] || match[3] ? 'strong'
                                            : match[4]             ? 's'
                                            : match[5]             ? 'em'
                                                                   : 'a');
        if (match[1])
            node.textContent = match[1];
        else if (match[6]) {
            node.textContent = match[6];
            node.href = match[7];
            node.onclick = e => {
                e.preventDefault();
                send({command : 'openLink', url : e.currentTarget.href});
            };
        } else
            inlineText(node, match[2] || match[3] || match[4] || match[5], depth + 1, budget);
        target.append(node);
        start = tokens.lastIndex;
    }
    target.append(document.createTextNode(text.slice(start)));
}
// Block parsing is bounded independently from the model's text limit. Deeper or oversized
// structures remain literal text, so a formatting limit never silently removes content.
function markdownCells(line) {
    const cells = [];
    let cell = '', ticks = 0;
    for (let i = 0; i < line.length; i++) {
        if (line[i] === '\\' && line[i + 1] === '|') {
            cell += '|';
            i++;
            continue;
        }
        if (line[i] === '`') {
            let end = i;
            while (line[end] === '`')
                end++;
            const run = end - i;
            ticks = ticks === run ? 0 : !ticks ? run : ticks;
            cell += line.slice(i, end);
            i = end - 1;
            continue;
        }
        if (line[i] === '|' && !ticks) {
            cells.push(cell.trim());
            cell = '';
        } else
            cell += line[i];
    }
    cells.push(cell.trim());
    if (!cells[0])
        cells.shift();
    if (!cells.at(-1))
        cells.pop();
    return cells;
}
function markdownBlocks(lines, budget, depth = 0) {
    const inline = (target, text) => inlineText(target, text, 0, budget);
    const out = document.createDocumentFragment();
    const bullet = line => line.match(/^( {0,3})([-+*]|\d{1,9}[.)])( +)(.*)$/),
          fence = line => line.match(/^ {0,3}(`{3,}|~{3,})([^`]*)$/),
          rule = line => /^ {0,3}(?:-\s*){3,}$/.test(line) || /^ {0,3}(?:\*\s*){3,}$/.test(line) ||
                         /^ {0,3}(?:_\s*){3,}$/.test(line),
          startsBlock = line =>
              !!(fence(line) || bullet(line) || rule(line) || /^ {0,3}(?:#{1,6}\s|>)/.test(line));
    let i = 0;
    while (i < lines.length) {
        if (!lines[i].trim()) {
            i++;
            continue;
        }
        if (depth > 8 || budget.left-- <= 0) {
            const literal = document.createElement('p');
            literal.className = 'literal-markdown';
            literal.append(textSpan(lines.slice(i).join('\n')));
            out.append(literal);
            break;
        }
        const line = lines[i], f = fence(line);
        const mathStart = line.trim(), mathEnd = mathStart === '\\[' ? '\\]' : '$$';
        if (mathStart === '\\[' || mathStart === '$$') {
            let end = i + 1;
            while (end < lines.length && end - i <= 64 && lines[end].trim() !== mathEnd)
                end++;
            const math = end < lines.length && lines[end].trim() === mathEnd &&
                         mathExpression(lines.slice(i + 1, end).join('\n'), true);
            if (math) {
                out.append(math);
                i = end + 1;
                continue;
            }
        }
        if (f) {
            const content = [], end = new RegExp('^ {0,3}' + f[1][0] + '{' + f[1].length + ',}\\s*$');
            i++;
            while (i < lines.length && !end.test(lines[i]))
                content.push(lines[i++]);
            const closed = i < lines.length;
            if (closed)
                i++;
            const source = content.join('\n'), language = f[2].trim(),
                  figure = language === 'overtone' ? renderReadingFigure(source) : null;
            out.append(figure || codeBlock(source, language, closed));
            continue;
        }
        let m = line.match(/^ {0,3}(#{1,6})\s+(.+)$/);
        if (m || i + 1 < lines.length && /^ {0,3}(?:=+|-+)\s*$/.test(lines[i + 1])) {
            const level = m                                ? Math.min(m[1].length + 1, 6)
                          : lines[i + 1].trim()[0] === '=' ? 2
                                                           : 3,
                  heading = document.createElement('h' + level);
            inline(heading, m ? m[2].replace(/\s+#+$/, '') : line.trim());
            out.append(heading);
            i += m ? 1 : 2;
            continue;
        }
        if (rule(line)) {
            out.append(document.createElement('hr'));
            i++;
            continue;
        }
        const columns = i + 1 < lines.length && line.includes('|') ? markdownCells(lines[i + 1]) : [];
        if (columns.length > 1 && columns.length <= 30 && columns.every(x => /^:?-{3,}:?$/.test(x)) &&
            markdownCells(line).length === columns.length) {
            const table = document.createElement('table'), header = document.createElement('thead'),
                  body = document.createElement('tbody');
            table.append(header, body);
            function row(value, heading) {
                const cells = markdownCells(value), tr = document.createElement('tr');
                for (let n = 0; n < columns.length; n++) {
                    const td = document.createElement(heading ? 'th' : 'td');
                    if (heading)
                        td.scope = 'col';
                    if (columns[n].endsWith(':'))
                        td.style.textAlign = columns[n][0] === ':' ? 'center' : 'right';
                    inline(td, cells[n] || '');
                    tr.append(td);
                }
                (heading ? header : body).append(tr);
                budget.left -= columns.length;
            }
            row(line, true);
            i += 2;
            while (i < lines.length && lines[i].trim() && lines[i].includes('|') && body.rows.length < 256 &&
                   budget.left >= columns.length && markdownCells(lines[i]).length <= columns.length)
                row(lines[i++], false);
            const scroll = document.createElement('div');
            scroll.className = 'table-scroll';
            scroll.append(table);
            out.append(scroll);
            continue;
        }
        m = bullet(line);
        if (m) {
            const ordered = /\d/.test(m[2]), indent = m[1].length,
                  list = document.createElement(ordered ? 'ol' : 'ul');
            if (ordered)
                list.start = parseInt(m[2], 10);
            while (i < lines.length) {
                if (budget.left <= 0) {
                    const li = document.createElement('li'), literal = document.createElement('p');
                    literal.className = 'literal-markdown';
                    literal.append(textSpan(lines.slice(i).join('\n')));
                    li.append(literal);
                    list.append(li);
                    i = lines.length;
                    break;
                }
                const item = bullet(lines[i]);
                if (!item || item[1].length !== indent || /\d/.test(item[2]) !== ordered || rule(lines[i]))
                    break;
                const content = [ item[4] ], contentIndent = item[1].length + item[2].length + item[3].length;
                i++;
                let blank = false;
                while (i < lines.length) {
                    const next = lines[i], nextItem = bullet(next), spaces = next.match(/^ */)[0].length;
                    if (nextItem && nextItem[1].length <= indent)
                        break;
                    if (!next.trim()) {
                        content.push('');
                        blank = true;
                        i++;
                        continue;
                    }
                    if (spaces >= contentIndent)
                        content.push(next.slice(contentIndent));
                    else if (!blank && !startsBlock(next))
                        content.push(next.trimStart());
                    else
                        break;
                    blank = false;
                    i++;
                }
                const li = document.createElement('li'), task = content[0].match(/^\[([ xX])\]\s+(.*)$/);
                if (task) {
                    content[0] = task[2];
                    li.className = 'task-item';
                }
                li.append(markdownBlocks(content, budget, depth + 1));
                if (task) {
                    const check = document.createElement('input');
                    check.type = 'checkbox';
                    check.disabled = true;
                    check.checked = task[1] !== ' ';
                    check.setAttribute('aria-label', tr(check.checked ? 'Completato' : 'Da completare'));
                    (li.firstElementChild || li).prepend(check);
                }
                list.append(li);
            }
            out.append(list);
            continue;
        }
        if (/^ {0,3}>/.test(line)) {
            const content = [];
            while (i < lines.length && /^ {0,3}>/.test(lines[i]))
                content.push(lines[i++].replace(/^ {0,3}> ?/, ''));
            const quote = document.createElement('blockquote');
            quote.append(markdownBlocks(content, budget, depth + 1));
            out.append(quote);
            continue;
        }
        if (/^ {4}/.test(line)) {
            const content = [];
            while (i < lines.length && (/^ {4}/.test(lines[i]) || !lines[i].trim()))
                content.push(lines[i++].slice(4));
            while (content.at(-1) === '')
                content.pop();
            out.append(codeBlock(content.join('\n'), '', true));
            continue;
        }
        const p = document.createElement('p');
        do {
            const value = lines[i++], hard = / {2,}$|\\$/.test(value);
            inline(p, value.replace(/ {2,}$|\\$/, ''));
            if (i >= lines.length || !lines[i].trim() || startsBlock(lines[i]))
                break;
            p.append(hard ? document.createElement('br') : document.createTextNode(' '));
        } while (i < lines.length);
        out.append(p);
    }
    return out;
}
function reconcileMarkdown(target, fragment) {
    const incoming = [...fragment.childNodes ], existing = [...target.childNodes ];
    for (let i = 0; i < incoming.length; i++) {
        const next = incoming[i], old = existing[i];
        if (old?.classList?.contains('code-block') && next.classList?.contains('code-block')) {
            updateCodeBlock(old, next);
            continue;
        }
        if (old?.isEqualNode(next)) {
            if ('copyValue' in next)
                old.copyValue = next.copyValue;
            continue;
        }
        if (old && old.nodeType === 3 && next.nodeType === 3 && next.data.startsWith(old.data)) {
            old.appendData(next.data.slice(old.data.length));
            continue;
        }
        if (old?.nodeType === 1 && next.nodeType === 1 && old.nodeName === next.nodeName &&
            old.className === next.className && !old.matches('svg,figure')) {
            for (const attr of [...old.attributes])
                if (!next.hasAttribute(attr.name))
                    old.removeAttribute(attr.name);
            for (const attr of next.attributes)
                if (old.getAttribute(attr.name) !== attr.value)
                    old.setAttribute(attr.name, attr.value);
            if (old.nodeName === 'A')
                old.onclick = next.onclick;
            if (old.nodeName === 'INPUT')
                old.checked = next.checked;
            reconcileMarkdown(old, next);
            continue;
        }
        if (old)
            old.replaceWith(next);
        else
            target.append(next);
    }
    for (let i = incoming.length; i < existing.length; i++)
        existing[i].remove();
}
function renderMarkdown(target, text) {
    const fragment = markdownBlocks(text.replace(/\r\n?/g, '\n').split('\n'), {left : 2048, inline : 2048}),
          first = fragment.firstElementChild;
    if (first?.tagName === 'P' && first.textContent.length <= 220)
        first.classList.add('response-lead');
    reconcileMarkdown(target, fragment);
    highlightVisibleCode();
}

// WebKit's selection gap painting can extend a selected line to the viewport,
// outside its reading frame. A native text-range highlight paints glyph runs only;
// selection/copy, keyboard navigation and accessibility stay owned by the browser.
onUIDocument(doc => {
    const view = doc.defaultView;
    if (typeof view.Highlight !== 'function' || !view.CSS.highlights) return;
    const highlight = new view.Highlight();
    view.CSS.highlights.set('overtone-selection', highlight);
    doc.documentElement.dataset.textHighlight = 'true';
    doc.addEventListener('selectionchange', () => {
        highlight.clear();
        const selection = doc.getSelection();
        if (selection?.rangeCount && !selection.isCollapsed)
            highlight.add(selection.getRangeAt(0));
    });
});
// Select All in a read-only dialog belongs to its document, not the inactive
// conversation behind it. Editable controls keep their native selection scope.
onUIEvent('keydown', event => {
    if (!(event.metaKey || event.ctrlKey) || event.key.toLowerCase() !== 'a' ||
        event.target.closest('input,textarea,[contenteditable=true]')) return;
    const content = document.querySelector('dialog:modal :is(.expanded-code,.legal-notices)');
    if (!content) return;
    event.preventDefault();
    const range = document.createRange(), selection = getSelection();
    range.selectNodeContents(content);
    selection.removeAllRanges();
    selection.addRange(range);
});

// Native copy uses rendered layout, which can turn paragraph margins, table
// spacing and code toolbars into clipboard text. Copy only the selected content
// tree, with semantic breaks; retain literal indentation/newlines inside code.
onUIEvent('copy', event => {
    if (!event.clipboardData || activeUIElement()?.matches('input,textarea,[contenteditable=true]'))
        return;
    const selection = event.currentTarget.getSelection();
    if (!selection?.rangeCount || selection.isCollapsed) return;
    const source = selection.getRangeAt(0), sourceDocument = source.startContainer.ownerDocument || source.startContainer,
          parts = [], modal = document.querySelector('dialog:modal');
    function text(fragment) {
        let value = '', separator = '';
        const boundary = token => { if (value) separator = token; };
        function visit(node) {
            if (node.nodeType === Node.TEXT_NODE) {
                if (node.textContent) {
                    if (separator && !value.endsWith('\n')) value += separator;
                    separator = '';
                    value += node.textContent;
                }
                return;
            }
            if (node.nodeType !== Node.ELEMENT_NODE && node.nodeType !== Node.DOCUMENT_FRAGMENT_NODE)
                return;
            if (node.matches?.('button,input,svg,.code-block>header,[aria-hidden=true]')) return;
            if (node.nodeName === 'BR') { value += '\n'; separator = ''; return; }
            const block = /^(P|H[1-6]|LI|UL|OL|PRE|BLOCKQUOTE|TR|DIV|SECTION|FIGURE)$/.test(node.nodeName);
            if (block) boundary('\n');
            for (const child of node.childNodes) visit(child);
            if (block) boundary('\n');
            else if (/^(TD|TH)$/.test(node.nodeName)) boundary('\t');
        }
        visit(fragment);
        return value;
    }
    for (const body of uiElements('.message-body,.reasoning-body,.expanded-code,.generated-documents')) {
        if (body.ownerDocument !== sourceDocument ||
            hiddenUIElement(body) || !body.getClientRects().length ||
            body.parentElement?.closest('.message-body,.reasoning-body,.expanded-code,.generated-documents') ||
            modal && !modal.contains(body) || !source.intersectsNode(body)) continue;
        const range = body.ownerDocument.createRange();
        range.selectNodeContents(body);
        if (source.compareBoundaryPoints(Range.START_TO_START, range) > 0)
            range.setStart(source.startContainer, source.startOffset);
        if (source.compareBoundaryPoints(Range.END_TO_END, range) < 0)
            range.setEnd(source.endContainer, source.endOffset);
        if (!range.collapsed) parts.push(text(range.cloneContents()));
    }
    if (parts.length) {
        event.preventDefault();
        event.clipboardData.setData('text/plain', parts.join('\n'));
    }
});

// Optional finite display grammar. Invalid descriptors remain ordinary code blocks.
// No model-supplied HTML, SVG, CSS, event handler or network resource is accepted.
function renderReadingFigure(source) {
    if (source.length > 8192)
        return null;
    let data;
    try {
        data = JSON.parse(source)
    } catch {
        return null
    }
    if (!data || typeof data !== 'object' || Array.isArray(data) || !Array.isArray(data.items))
        return null;
    const label = (value, limit) => typeof value === 'string' && value.length > 0 && value.length <= limit;
    if (data.caption !== undefined && (!label(data.caption, 400)))
        return null;
    const figure = document.createElement('figure');
    if (data.type === 'flow') {
        if (data.items.length < 2 || data.items.length > 4 ||
            data.items.some(x => !x || !label(x.title, 64) ||
                                 (x.description !== undefined && !label(x.description, 140))))
            return null;
        const list = document.createElement('div');
        list.className = 'reading-flow';
        list.setAttribute('role', 'list');
        for (const item of data.items) {
            const step = document.createElement('div');
            step.setAttribute('role', 'listitem');
            const title = document.createElement('strong');
            title.append(textSpan(item.title));
            step.append(title);
            if (item.description) {
                const text = document.createElement('small');
                text.append(textSpan(item.description));
                step.append(text)
            }
            list.append(step)
        }
        figure.append(list);
    } else if (data.type === 'bars') {
        if (data.items.length < 1 || data.items.length > 12 ||
            (data.unit !== undefined && !label(data.unit, 24)) ||
            data.items.some(x => !x || !label(x.label, 80) || typeof x.value !== 'number' ||
                                 !Number.isFinite(x.value) || x.value < 0))
            return null;
        const max = Math.max(1, ...data.items.map(x => x.value)), chart = document.createElement('div');
        chart.className = 'reading-bars';
        for (const item of data.items) {
            const row = document.createElement('div');
            row.className = 'reading-bar';
            const name = document.createElement('span');
            name.append(textSpan(item.label));
            const track = document.createElement('div');
            track.className = 'bar-track';
            track.setAttribute('aria-hidden', 'true');
            const fill = document.createElement('span');
            fill.className = 'bar-fill';
            fill.style.width = (item.value / max * 100) + '%';
            track.append(fill);
            const value = document.createElement('output');
            value.append(textSpan(String(item.value) + (data.unit ? ' ' + data.unit : '')));
            row.append(name, track, value);
            chart.append(row)
        }
        figure.append(chart);
    } else
        return null;
    if (data.caption) {
        const caption = document.createElement('figcaption');
        caption.className = 'reading-caption';
        caption.append(textSpan(data.caption));
        figure.append(caption)
    }
    return figure;
}
