'use strict';
// Only the selected local catalogue is read by the platform, before UI scripts run.
let language = window.overtoneLocale?.language || 'it',
    translationCatalog = window.overtoneLocale?.strings || {}, appliedCatalog, pluralRules;
let countFormat = new Intl.NumberFormat();
let decimalFormat = new Intl.NumberFormat(undefined, {maximumFractionDigits:1});
const tr = text => translationCatalog[text] || text;
// Mark only authored labels. Never infer translations from user/model content.
function localLabel(tag, key, className = '') {
    const element = document.createElement(tag);
    element.dataset.textKey = key;
    element.textContent = tr(key);
    if (className) element.className = className;
    return element;
}
function localAccessible(element, key) {
    element.dataset.labelKey = key;
    element.setAttribute('aria-label', tr(key));
    if (element.hasAttribute('data-tip')) element.dataset.tip = tr(key);
}
function quantity(kind, count) {
    const category = (pluralRules ||= new Intl.PluralRules(language)).select(count), key = '@' + kind + '.';
    return (translationCatalog[key + category] || translationCatalog[key + 'other'] || '{n}')
        .replace('{n}', countFormat.format(count));
}
window.overtoneSetLocale = (code, strings) => {
    window.overtoneLocale = {language:code,strings};
    if (window.overtoneReceive && current) window.overtoneReceive(current);
};
// Region comes from the OS, independently of the application's translation. No network lookup.
let formatLocale, formatRevision = 0, regionalKey = '', regionalPreferences = {};
const timestampFormats = new Map();
function setRegionalPreferences(p) {
    regionalPreferences = p;
    const os = window.overtoneRegion || {}, region = p.region || 'auto';
    const locale = region === 'auto' ? os.locale || undefined : region;
    const key = JSON.stringify([locale, p.dateFormat, p.hourFormat, os.hourCycle]);
    if (key === regionalKey)
        return false;
    regionalKey = key;
    try { formatLocale = new Intl.NumberFormat(locale).resolvedOptions().locale; }
    catch { formatLocale = undefined; }
    countFormat = new Intl.NumberFormat(formatLocale);
    decimalFormat = new Intl.NumberFormat(formatLocale, {maximumFractionDigits:1});
    timestampFormats.clear();
    formatRevision++;
    return true;
}
function formatTimestamp(seconds, {seconds: showSeconds = false, alwaysDate = false, now = Date.now()} = {}) {
    const date = new Date(seconds * 1000), today = new Date(now);
    if (!(seconds > 0) || !Number.isFinite(date.getTime()))
        return '—';
    const withDate = alwaysDate || date.getFullYear() !== today.getFullYear() ||
        date.getMonth() !== today.getMonth() || date.getDate() !== today.getDate();
    const key = [withDate, showSeconds].join();
    let formats = timestampFormats.get(key);
    if (!formats) {
        const p = regionalPreferences, options = {hour: '2-digit', minute: '2-digit'};
        if (showSeconds) options.second = '2-digit';
        const cycle = p.hourFormat === '12' ? 'h12' : p.hourFormat === '24' ? 'h23'
            : (!p.region || p.region === 'auto') ? window.overtoneRegion?.hourCycle : undefined;
        if (cycle) options.hourCycle = cycle;
        const clock = new Intl.DateTimeFormat(formatLocale, options);
        const day = new Intl.DateTimeFormat(formatLocale, {year: 'numeric', month: '2-digit', day: '2-digit'});
        const orderedDay = new Intl.DateTimeFormat('en-GB', {
            year: 'numeric', month: '2-digit', day: '2-digit', calendar: 'gregory', numberingSystem: 'latn'
        });
        formats = {clock, day, orderedDay};
        timestampFormats.set(key, formats);
    }
    const time = formats.clock.format(date);
    if (!withDate) return time;
    const order = regionalPreferences.dateFormat;
    let day;
    if (['dmy', 'mdy', 'ymd'].includes(order)) {
        const parts = Object.fromEntries(formats.orderedDay.formatToParts(date).map(p => [p.type, p.value]));
        day = (order === 'ymd' ? [parts.year, parts.month, parts.day]
            : order === 'mdy' ? [parts.month, parts.day, parts.year]
            : [parts.day, parts.month, parts.year]).join(order === 'ymd' ? '-' : '/');
    } else day = formats.day.format(date);
    return day + ' · ' + time;
}
function setTimestamp(node, seconds, showSeconds = false) {
    node.dataset.timestamp = seconds || 0;
    node.dataset.seconds = String(showSeconds);
    const date = new Date(seconds * 1000);
    if (seconds > 0 && Number.isFinite(date.getTime())) node.dateTime = date.toISOString();
    else node.removeAttribute('datetime');
    const text = formatTimestamp(seconds, {seconds: showSeconds});
    if (node.textContent !== text) node.textContent = text;
}
function refreshTimestamps() {
    for (const node of uiElements('time[data-timestamp]'))
        setTimestamp(node, Number(node.dataset.timestamp), node.dataset.seconds === 'true');
}
function regionalPreview() {
    const region = document.querySelector('[data-pref="region"]');
    if (region) {
        const locale = window.overtoneRegion?.locale || new Intl.DateTimeFormat().resolvedOptions().locale;
        let name = locale;
        try {
            const country = new Intl.Locale(locale).region;
            name = new Intl.DisplayNames([language], {type: country ? 'region' : 'language'}).of(country || locale);
        } catch {}
        region.options[0].textContent = tr('Sistema') + ' · ' + name;
    }
    const preview = document.getElementById('regional-preview');
    if (preview) preview.textContent = formatTimestamp(Date.now() / 1000, {alwaysDate: true}) +
        ' · ' + (12345.67).toLocaleString(formatLocale);
}
let regionalTimer, regionalClockKey = '';
function refreshRegionalClock(force = false) {
    clearTimeout(regionalTimer);
    if (!document.hidden) {
        const today = new Date(), key = [today.toDateString(), today.getTimezoneOffset(),
            new Intl.DateTimeFormat().resolvedOptions().timeZone].join();
        if (force || key !== regionalClockKey) {
            regionalClockKey = key;
            timestampFormats.clear();
            formatRevision++;
            refreshTimestamps();
            window.refreshRegionalViews?.();
        }
        regionalPreview();
        const next = new Date();
        next.setHours(24, 0, 0, 50);
        regionalTimer = setTimeout(() => refreshRegionalClock(), next.getTime() - Date.now());
    }
}
window.setRegionalEnvironment = environment => {
    if (JSON.stringify(environment) === JSON.stringify(window.overtoneRegion)) return;
    window.overtoneRegion = environment;
    regionalKey = '';
    setRegionalPreferences(regionalPreferences);
    refreshRegionalClock(true);
};
window.addEventListener('focus', () => refreshRegionalClock());
document.addEventListener('visibilitychange', () => refreshRegionalClock());
const staticText = [];
const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
while (walker.nextNode()) {
    const node = walker.currentNode, key = node.textContent.trim();
    if (Object.hasOwn(translationCatalog,key) &&
        !node.parentElement.closest('#title,#phase,#engine-status,#memory-label,script,style,[translate=no]'))
        staticText.push([ node, key ]);
}
const staticAttributes = [];
for (const node of document.querySelectorAll('[aria-label],[placeholder],[data-tip]'))
    for (const attribute of ['aria-label', 'placeholder', 'data-tip']) {
        const text = node.getAttribute(attribute);
        if (Object.hasOwn(translationCatalog,text))
            staticAttributes.push([ node, attribute, text ]);
    }
function setLanguage(value) {
    const locale = window.overtoneLocale;
    if (!locale || locale.language !== value || (value === language && locale.strings === appliedCatalog))
        return false;
    language = value;
    pluralRules = new Intl.PluralRules(value);
    appliedCatalog = translationCatalog = locale.strings;
    document.documentElement.lang = value;
    for (const [node, key] of staticText)
        node.textContent = tr(key);
    for (const [node, attribute, text] of staticAttributes)
        node.setAttribute(attribute, tr(text));
    for (const node of uiElements('[data-text-key]'))
        node.textContent = tr(node.dataset.textKey);
    for (const node of uiElements('[data-label-key]'))
        localAccessible(node, node.dataset.labelKey);
    window.refreshLibrary?.();
    window.refreshShellLanguage?.();
    regionalPreview();
    return true;
}
refreshRegionalClock();
