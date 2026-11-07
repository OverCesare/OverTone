'use strict';
// One perceptual palette owns surfaces, ink and selections. The approved mark is independent.
// Large surfaces carry less chroma; derived materials retain the chosen hue.
window.overtonePalette = (name, dark) => {
    const tones = {
        purple : '#5f55ec',
        blue : '#245fe8',
        teal : '#007e83',
        green : '#168343',
        amber : '#d47800',
        rose : '#d53e78'
    };
    const base = (tones[name] || tones.purple).slice(1).match(/../g).map(x => parseInt(x, 16) / 255);
    const linear = x => x <= .04045 ? x / 12.92 : ((x + .055) / 1.055) ** 2.4;
    const gamma = x => x <= .0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - .055;
    const [r, g, b] = base.map(linear);
    const l = Math.cbrt(.4122214708 * r + .5363325363 * g + .0514459929 * b),
          m = Math.cbrt(.2119034982 * r + .6806995451 * g + .1073969566 * b),
          s = Math.cbrt(.0883024619 * r + .2817188376 * g + .6299787005 * b);
    const light = .2104542553 * l + .793617785 * m - .0040720468 * s,
          a = 1.9779984951 * l - 2.428592205 * m + .4505937099 * s,
          z = .0259040371 * l + .7827717662 * m - .808675766 * s, chroma = Math.hypot(a, z),
          hue = Math.atan2(z, a);
    function rgb(L, C) {
        const A = C * Math.cos(hue), B = C * Math.sin(hue), x = (L + .3963377774 * A + .2158037573 * B) ** 3,
              y = (L - .1055613458 * A - .0638541728 * B) ** 3,
              z = (L - .0894841775 * A - 1.291485548 * B) ** 3;
        return [
            4.0767416621 * x - 3.3077115913 * y + .2309699292 * z,
            -1.2684380046 * x + 2.6097574011 * y - .3413193965 * z,
            -.0041960863 * x - .7034186147 * y + 1.707614701 * z
        ];
    }
    function shade(L, C) {
        let low = 0, high = C, value = rgb(L, C);
        if (value.some(x => x < 0 || x > 1)) {
            for (let i = 0; i < 16; i++) {
                const candidate = (low + high) / 2, test = rgb(L, candidate);
                if (test.every(x => x >= 0 && x <= 1))
                    low = candidate;
                else
                    high = candidate;
            }
            value = rgb(L, low);
        }
        return '#' +
               value
                   .map(
                       x =>
                           Math.round(Math.min(1, Math.max(0, gamma(x))) * 255).toString(16).padStart(2, '0'))
                   .join('');
    }
    const calm = dark && ['green', 'amber', 'rose'].includes(name) ? .65 : 1;
    // Interaction materials are separated from the tinted raised surface, not
    // just from the white workspace. Changing its lightness preserves that gap.
    const raisedLight = dark ? .245 : .977,
          hoverLight = raisedLight + (dark ? .095 : -.065);
    const result = dark ? {
        background : shade(.175, name === 'purple' ? .036 : .027 * calm),
        panel : shade(.215, name === 'purple' ? .038 : .029 * calm),
        raised : shade(raisedLight, name === 'purple' ? .044 : .033 * calm),
        line : shade(.365, .046 * calm),
        control : shade(.61, .055),
        text : shade(.955, .012),
        muted : shade(.78, .028),
        selected : shade(.32, .105 * calm),
        accent : shade(Math.max(.575, light), chroma),
        'accent-ink' : shade(.79, chroma),
        'on-accent' : '#ffffff',
        focus : shade(.79, chroma)
    }
                        : {
                              background : shade(.967, name === 'purple' ? .03 : .012),
                              panel : '#ffffff',
                              raised : shade(raisedLight, name === 'purple' ? .024 : .007),
                              line : shade(.86, .015),
                              control : shade(.57, .025),
                              text : shade(.30, .008),
                              muted : shade(.47, .03),
                              selected : shade(.958, .018),
                              accent : tones[name] || tones.purple,
                              'accent-ink' : shade(Math.min(.49, light), chroma),
                              'on-accent' : '#ffffff',
                              focus : shade(Math.min(.49, light), chroma)
                          };
    if (name === 'purple') {
        result['accent-ink'] = result.focus = dark ? '#aaa1ff' : '#5145ce';
        if (dark) result.selected = '#2b2854';
    }
    const luminance = hex =>
        hex.slice(1)
            .match(/../g)
            .map(x => linear(parseInt(x, 16) / 255))
            .reduce((sum, value, index) => sum + value * [ .2126, .7152, .0722 ][index], 0);
    // Filled controls share white ink in every tone. Find the lightest matching fill;
    // leave vivid progress colours untouched rather than switching some controls to black.
    const fillChroma = chroma * (dark || name === 'purple' ? 1 : .78);
    let lower = 0, upper = 1;
    for (let i = 0; i < 12; i++) {
        const candidate = (lower + upper) / 2;
        if (1.05 / (luminance(shade(candidate, fillChroma)) + .05) >= 4.6)
            lower = candidate;
        else
            upper = candidate;
    }
    result['on-accent'] = '#ffffff';
    result['accent-fill'] = shade(lower, fillChroma);
    result['accent-fill-hover'] = `color-mix(in srgb, #000000 8%, ${result['accent-fill']})`;
    Object.assign(result, dark ? {
        hover: shade(hoverLight, .085 * calm), 'hover-ink': result.text,
        'selection-fill': shade(.425, .12 * calm), 'selection-ink': result.text,
        'selected-hover': shade(.46, .13 * calm), 'selected-hover-ink': result.text,
        'row-selection': shade(.425, .12 * calm), 'row-selected-hover': shade(.46, .13 * calm)
    } : {
        // Filled compact selections and bordered list selections are different
        // roles. A large list row must not become a saturated highlighter block.
        hover: shade(hoverLight, .045), 'hover-ink': result.text,
        'selection-fill': result['accent-fill'], 'selection-ink': '#ffffff',
        'selected-hover': result['accent-fill-hover'], 'selected-hover-ink': '#ffffff',
        'selection-edge': result['accent-fill'],
        'row-selection': shade(.94, .035), 'row-selected-hover': shade(.89, .055)
    });
    result['user-surface'] = dark ? result.selected : shade(.92, .045);
    result['user-edge'] = dark ? 'transparent' : shade(.855, .04);
    result['text-selection'] = dark ? shade(.43, .105 * calm) : shade(.84, .075);
    result['text-selection-ink'] = dark ? '#ffffff' : shade(.25, .015);
    result['status-surface'] = dark ? result.raised : '#fbfbfd';
    return result;
};
let appearanceKey = '', appearanceGeneration = 0;
window.updateChromeAppearance = () => {
    const root = document.documentElement,
          dark = root.dataset.theme === 'dark' ||
                 root.dataset.theme !== 'light' && (root.dataset.systemTheme
                     ? root.dataset.systemTheme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches),
          key = [ dark, root.dataset.accent, root.dataset.chrome ].join(':');
    if (key === appearanceKey)
        return;
    appearanceKey = key;
    const generation = ++appearanceGeneration;
    root.classList.add('appearance-changing');
    requestAnimationFrame(() => requestAnimationFrame(() => {
        if (generation === appearanceGeneration)
            root.classList.remove('appearance-changing');
    }));
    const p = window.overtonePalette(root.dataset.accent, dark);
    root.style.colorScheme = dark ? 'dark' : 'light';
    for (const [key, value] of Object.entries(p))
        root.style.setProperty('--' + key, value);
    root.style.setProperty('--workspace-surface', p.panel);
    root.style.setProperty('--rail-surface', dark ? p.raised : p.background);
    root.style.setProperty('--accent-edge', p['selection-edge'] || p.accent);
    root.style.setProperty('--edge-shadow', dark ? '#00000035' : '#17171f10');
    root.style.setProperty('--shadow', dark ? '0 18px 60px #0006' : '0 18px 60px #17171f20');
    root.style.setProperty('--danger', dark ? '#ff989f' : '#b42335');
    root.style.setProperty('--danger-surface', dark ? '#4a1822' : '#ffe5e9');
    root.style.setProperty('--danger-fill', '#c1273f');
    root.style.setProperty('--on-danger', '#ffffff');
    root.style.setProperty('--status-ready', dark ? '#60d99b' : '#177a46');
    root.style.setProperty('--status-working', dark ? '#ffc05b' : '#966000');
    root.style.setProperty('--status-idle', dark ? '#87baff' : '#2863bd');
    root.style.setProperty('--card-shadow', dark ? '0 2px 5px #0003' : '0 2px 5px #17171f0b');
    root.style.setProperty('--tooltip-surface', dark ? '#34343c' : '#fbfbfd');
    root.style.setProperty('--tooltip-edge', dark ? '#62626f' : '#b8b8c3');
    root.style.setProperty('--code-surface', dark ? '#222329' : '#f4f4f7');
    root.style.setProperty('--code-ink', dark ? '#ededef' : '#303039');
    root.style.setProperty('--code-edge', dark ? '#46464f' : '#d4d4dc');
    root.style.setProperty('--syntax-keyword', dark ? '#b8acff' : '#6736b8');
    root.style.setProperty('--syntax-directive', dark ? '#efabcf' : '#9e2861');
    root.style.setProperty('--syntax-function', dark ? '#82c9f0' : '#17618c');
    root.style.setProperty('--syntax-comment', dark ? '#a3acbc' : '#616b7b');
    root.style.setProperty('--syntax-string', dark ? '#88d7a9' : '#176944');
    root.style.setProperty('--syntax-number', dark ? '#efbd80' : '#925007');
    for (const swatch of document.querySelectorAll('.color-swatch')) {
        const name = [...swatch.classList ].find(x => x.startsWith('swatch-'))?.slice(7);
        if (name)
            swatch.style.background = window.overtonePalette(name, false).accent;
        if (!swatch.parentElement.querySelector('.swatch-check')) {
            const check = icon('check');
            check.classList.add('swatch-check');
            swatch.parentElement.append(check);
        }
    }
    if (root.dataset.chrome === 'mac') {
        const background = [ 1, 3, 5 ].map(i => parseInt(p.background.slice(i, i + 2), 16) / 255);
        send({command : 'appearance', background});
    }
};
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', window.updateChromeAppearance);
window.updateChromeAppearance();
