'use strict';

const VAT_RATES = [0, 5, 7, 10, 20, 22];
const KINDS = {product: 'Продукция', semi: 'Полуфабрикат', raw: 'Сырьё'};
const UNITS = {kg: 'кг', l: 'л', pcs: 'шт'};

/**
 * Запрос к API сервера; при ошибке бросает Error с текстом из ответа.
 */
// вошедший пользователь {id, login, name, role}; null - не вошёл
let me = null;

const ROLES = {admin: 'Администратор', technologist: 'Технолог', seller: 'Продавец'};

/** Технолог или администратор: меняет справочники, цены, организацию, закрывает период и перепроводит. */
const canManage = () => Boolean(me && me.role !== 'seller');

/**
 * Только просмотр: выключает поля ввода и убирает кнопки в root, кроме перечисленных по тексту.
 */
function readOnly(root, keep = []) {
    root.querySelectorAll('input, select, textarea').forEach(e => { e.disabled = true; });
    root.querySelectorAll('button').forEach(b => {
        if (!keep.includes(b.textContent.trim())) {
            b.remove();
        }
    });
}

async function api(method, path, body) {
    const res = await fetch('/api/' + path, {
        method,
        headers: body !== undefined ? {'Content-Type': 'application/json'} : {},
        body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    const data = await res.json().catch(() => ({}));
    if (res.status === 401 && !path.startsWith('auth')) {
        // сессия истекла или пользователя заблокировали
        me = null;
        renderLogin(false);
    }
    if (!res.ok) {
        throw new Error(data && data.error ? data.error : res.status + ' ' + res.statusText);
    }
    if (method !== 'GET' && !path.startsWith('repost') && !path.startsWith('auth')) {
        // любое изменение могло быть задним числом
        refreshRepostBanner();
    }
    return data;
}

/**
 * Создаёт элемент: el('td', {class: 'num'}, 'текст', другойЭлемент).
 * Строки вставляются как текст, не как HTML.
 */
function el(tag, attrs, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs || {})) {
        if (key.startsWith('on')) {
            node.addEventListener(key.slice(2), value);
        } else if (value === true) {
            node.setAttribute(key, '');
        } else if (value !== false && value !== null && value !== undefined) {
            node.setAttribute(key, value);
        }
    }
    for (const child of children.flat(Infinity)) {
        if (child !== null && child !== undefined && child !== false) {
            node.append(child instanceof Node ? child : String(child));
        }
    }
    return node;
}

/**
 * Заменяет содержимое view; null, undefined и false пропускаются, как в el().
 * replaceChildren сам вставил бы их текстом "null".
 */
function render(view, ...children) {
    view.replaceChildren(...children.flat(Infinity).filter(c => c !== null && c !== undefined && c !== false));
}

function options(map, selected) {
    return Object.entries(map).map(([value, text]) => el('option', {value, selected: value === selected}, text));
}

/**
 * Количество в тысячных долях из строки "1,5" или "1.5" без потерь округления.
 * Возвращает null, если строка не число с не более чем тремя знаками после запятой.
 */
function parseQty(text) {
    const m = String(text).trim().match(/^(\d{1,7})(?:[.,](\d{1,3}))?$/);
    if (!m) {
        return null;
    }
    return Number(m[1]) * 1000 + Number((m[2] || '').padEnd(3, '0'));
}

/** Тысячные доли в строку с запятой: 1500 -> "1,5". */
function formatQty(milli) {
    const whole = Math.trunc(milli / 1000);
    const frac = String(Math.abs(milli % 1000)).padStart(3, '0').replace(/0+$/, '');
    return frac ? `${whole},${frac}` : String(whole);
}

/**
 * Копейки из строки "45", "45,5" или "45.50"; null, если не сумма в рублях с не более чем двумя знаками.
 */
function parseMoney(text) {
    const m = String(text).trim().replace(/\s/g, '').match(/^(\d{1,9})(?:[.,](\d{1,2}))?$/);
    if (!m) {
        return null;
    }
    return Number(m[1]) * 100 + Number((m[2] || '').padEnd(2, '0'));
}

/** Копейки в рубли с разделением разрядов: 1234550 -> "12 345,50". */
function formatMoney(kop) {
    const sign = kop < 0 ? '-' : '';
    const abs = Math.abs(kop);
    const rub = Math.trunc(abs / 100).toString().replace(/\B(?=(\d{3})+(?!\d))/g, '\u00a0');
    return `${sign}${rub},${String(abs % 100).padStart(2, '0')}`;
}

/** Склонение по числу: plural(21, ['рубль', 'рубля', 'рублей']) -> 'рубль'. */
function plural(n, forms) {
    const n10 = n % 10;
    const n100 = n % 100;
    if (n10 === 1 && n100 !== 11) {
        return forms[0];
    }
    if (n10 >= 2 && n10 <= 4 && (n100 < 10 || n100 >= 20)) {
        return forms[1];
    }
    return forms[2];
}

/**
 * Сумма прописью для печатных форм: 124800 -> "Одна тысяча двести сорок восемь рублей 00 копеек".
 */
function moneyInWords(kop) {
    const ones = [['', 'один', 'два', 'три', 'четыре', 'пять', 'шесть', 'семь', 'восемь', 'девять'],
        ['', 'одна', 'две', 'три', 'четыре', 'пять', 'шесть', 'семь', 'восемь', 'девять']];
    const teens = ['десять', 'одиннадцать', 'двенадцать', 'тринадцать', 'четырнадцать', 'пятнадцать', 'шестнадцать',
        'семнадцать', 'восемнадцать', 'девятнадцать'];
    const tens = ['', '', 'двадцать', 'тридцать', 'сорок', 'пятьдесят', 'шестьдесят', 'семьдесят', 'восемьдесят', 'девяносто'];
    const hundreds = ['', 'сто', 'двести', 'триста', 'четыреста', 'пятьсот', 'шестьсот', 'семьсот', 'восемьсот', 'девятьсот'];
    // разряды: [формы, женский род]
    const scales = [[['', '', ''], 0], [['тысяча', 'тысячи', 'тысяч'], 1], [['миллион', 'миллиона', 'миллионов'], 0],
        [['миллиард', 'миллиарда', 'миллиардов'], 0]];
    const triple = (n, female) => {
        const words = [hundreds[Math.floor(n / 100)]];
        const rest = n % 100;
        if (rest >= 10 && rest < 20) {
            words.push(teens[rest - 10]);
        } else {
            words.push(tens[Math.floor(rest / 10)], ones[female][rest % 10]);
        }
        return words.filter(Boolean).join(' ');
    };
    let rub = Math.floor(Math.abs(kop) / 100);
    const k = Math.abs(kop) % 100;
    const parts = [];
    for (let i = 0; rub > 0 && i < scales.length; i++, rub = Math.floor(rub / 1000)) {
        const n = rub % 1000;
        if (n) {
            const [forms, female] = scales[i];
            parts.unshift([triple(n, i === 0 ? 0 : female), plural(n, forms)].filter(Boolean).join(' '));
        }
    }
    const rubles = Math.floor(Math.abs(kop) / 100);
    let text = (parts.join(' ') || 'ноль') + ' ' + plural(rubles, ['рубль', 'рубля', 'рублей']);
    text += ` ${String(k).padStart(2, '0')} ${plural(k, ['копейка', 'копейки', 'копеек'])}`;
    return (kop < 0 ? 'минус ' : '') + text.charAt(0).toUpperCase() + text.slice(1);
}

function showMessage(text, kind = 'error') {
    const box = document.getElementById('message');
    box.textContent = text;
    box.className = kind;
    box.hidden = !text;
}

/** Выполняет действие и показывает ошибку вместо исключения. */
async function attempt(action, okText) {
    try {
        await action();
        showMessage(okText || '', 'ok');
        return true;
    } catch (e) {
        showMessage(e.message);
        return false;
    }
}

// ---------- номенклатура: список ----------

// ---------- группы ----------

/** Группы в порядке дерева: [{group, depth}], соседи по названию. */
function groupTree(groups) {
    const children = new Map();
    for (const g of groups) {
        const key = g.parent_id ?? 0;
        children.set(key, [...(children.get(key) || []), g]);
    }
    const out = [];
    const walk = (parent, depth) => {
        for (const g of (children.get(parent) || []).sort((a, b) => a.name.localeCompare(b.name, 'ru'))) {
            out.push({group: g, depth});
            walk(g.id, depth + 1);
        }
    };
    walk(0, 0);
    return out;
}

/** Путь группы: "Кондитерка / Торты". */
function groupPath(groups, id) {
    const byId = new Map(groups.map(g => [g.id, g]));
    const names = [];
    for (let g = byId.get(id); g && names.length < 100; g = byId.get(g.parent_id)) {
        names.unshift(g.name);
    }
    return names.join(' / ');
}

/** Группа и все её подгруппы. */
function groupWithDescendants(groups, id) {
    const out = new Set([id]);
    let grown = true;
    while (grown) {
        grown = false;
        for (const g of groups) {
            if (g.parent_id !== null && out.has(g.parent_id) && !out.has(g.id)) {
                out.add(g.id);
                grown = true;
            }
        }
    }
    return out;
}

/** <option> для выбора группы с отступами по вложенности; exclude - id, которых не показывать. */
function groupOptions(groups, selected, emptyText, exclude = new Set()) {
    return [
        el('option', {value: '', selected: selected === null || selected === undefined}, emptyText),
        ...groupTree(groups).filter(({group}) => !exclude.has(group.id)).map(({group, depth}) =>
            el('option', {value: group.id, selected: group.id === selected}, '   '.repeat(depth) + group.name)),
    ];
}

const selectValue = select => (select.value ? Number(select.value) : null);

// ---------- номенклатура: список ----------

const listState = {kind: '', archived: false, group: null};

/**
 * Дерево групп слева от списка: выбор группы, создание подгруппы, переименование и перенос.
 */
function groupsPanel(groups) {
    const selected = listState.group;
    const node = (id, text, depth) => el('div', {
        class: 'tree-node' + (selected === id ? ' active' : ''),
        style: `padding-left: ${8 + depth * 16}px`,
        onclick: () => { listState.group = id; route(); },
    }, text);

    const editor = el('div', {class: 'panel', hidden: true});
    const openEditor = () => {
        const g = groups.find(x => x.id === selected);
        const name = el('input', {value: g.name, size: 20});
        const parent = el('select', {}, groupOptions(groups, g.parent_id, '— верхний уровень —', groupWithDescendants(groups, g.id)));
        editor.replaceChildren(
            el('label', {}, 'Название', name),
            el('label', {}, 'Входит в', parent),
            el('div', {class: 'toolbar', style: 'margin-top: 8px'},
                el('button', {class: 'primary', onclick: async () => {
                    if (await attempt(() => api('PUT', `groups/${g.id}`, {name: name.value, parent_id: selectValue(parent)}), 'Группа сохранена')) {
                        route();
                    }
                }}, 'Сохранить'),
                el('button', {class: 'danger', onclick: async () => {
                    if (confirm(`Удалить группу «${g.name}»?`) && await attempt(() => api('DELETE', `groups/${g.id}`), 'Группа удалена')) {
                        listState.group = null;
                        route();
                    }
                }}, 'Удалить'),
                el('button', {onclick: () => { editor.hidden = true; }}, 'Отмена')));
        editor.hidden = false;
    };
    const addGroup = async () => {
        const where = selected ? `в «${groupPath(groups, selected)}»` : 'на верхнем уровне';
        const name = prompt(`Новая группа ${where}:`);
        if (!name) {
            return;
        }
        let created;
        if (await attempt(async () => { created = await api('POST', 'groups', {name, parent_id: selected}); }, 'Группа создана')) {
            listState.group = created.id;
            route();
        }
    };

    return el('aside', {class: 'groups'},
        el('div', {class: 'tree'},
            node(null, 'Все группы', 0),
            groupTree(groups).map(({group, depth}) => node(group.id, group.name, depth + 1))),
        el('div', {class: 'toolbar', style: 'margin-top: 8px'},
            el('button', {onclick: addGroup}, '+ группа'),
            selected ? el('button', {onclick: openEditor}, 'Изменить') : null),
        editor);
}

async function renderItems(view) {
    const query = new URLSearchParams();
    if (listState.kind) {
        query.set('kind', listState.kind);
    }
    if (listState.archived) {
        query.set('archived', '1');
    }
    const groups = await api('GET', 'groups');
    if (listState.group && !groups.some(g => g.id === listState.group)) {
        listState.group = null;
    }
    if (listState.group) {
        query.set('group', listState.group);
    }
    const items = await api('GET', 'items?' + query);

    const tabs = el('div', {class: 'tabs toolbar'},
        Object.entries({'': 'Все', ...KINDS}).map(([kind, text]) =>
            el('button', {
                class: listState.kind === kind ? 'active' : null,
                onclick: () => { listState.kind = kind; route(); },
            }, text)),
        el('label', {class: 'check'},
            el('input', {
                type: 'checkbox', checked: listState.archived,
                onchange: e => { listState.archived = e.target.checked; route(); },
            }),
            'показать архив'));

    const name = el('input', {name: 'name', placeholder: 'Название', required: true, size: 24});
    const kind = el('select', {name: 'kind'}, options(KINDS, listState.kind || 'product'));
    const unit = el('select', {name: 'unit'}, options(UNITS, 'pcs'));
    const group = el('select', {name: 'group'}, groupOptions(groups, listState.group, '— без группы —'));
    const form = el('form', {
        class: 'inline',
        onsubmit: async e => {
            e.preventDefault();
            let created;
            if (await attempt(async () => {
                created = await api('POST', 'items', {name: name.value, kind: kind.value, unit: unit.value, group_id: selectValue(group)});
            }, '')) {
                location.hash = '#/items/' + created.id;
            }
        },
    }, name, kind, unit, group, el('button', {class: 'primary'}, 'Добавить'));

    const rows = items.map(item => el('tr', {
            class: 'link' + (item.archived ? ' archived' : ''),
            onclick: () => { location.hash = '#/items/' + item.id; },
        },
        el('td', {}, item.name, item.archived ? el('span', {class: 'muted'}, ' (архив)') : null),
        el('td', {}, KINDS[item.kind]),
        el('td', {}, UNITS[item.unit]),
        el('td', {class: 'muted'}, item.group_id ? groupPath(groups, item.group_id) : ''),
        el('td', {}, item.kind === 'raw' ? '' : item.has_recipe ? 'есть' : el('span', {class: 'muted'}, 'нет'))));

    const panel = groupsPanel(groups);
    if (!canManage()) {
        panel.querySelector('.toolbar').remove();
    }
    render(view,
        el('h2', {}, 'Номенклатура'),
        canManage() ? form : null,
        el('div', {class: 'split'},
            panel,
            el('div', {class: 'split-main'},
                el('h3', {style: 'margin-top: 0'}, listState.group ? groupPath(groups, listState.group) : 'Все группы'),
                tabs,
                el('div', {class: 'table-wrap'}, el('table', {},
                    el('thead', {}, el('tr', {}, el('th', {}, 'Название'), el('th', {}, 'Вид'), el('th', {}, 'Ед.'), el('th', {}, 'Группа'), el('th', {}, 'Рецептура'))),
                    el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', {colspan: 5, class: 'muted'}, 'Пусто'))))))));
}

// ---------- номенклатура: карточка ----------

async function renderItem(view, id) {
    const [item, recipe, all, groups] = await Promise.all([
        api('GET', `items/${id}`),
        api('GET', `items/${id}/recipe`),
        api('GET', 'items?archived=1'),
        api('GET', 'groups'),
    ]);

    const name = el('input', {value: item.name, size: 30});
    const kind = el('select', {}, options(KINDS, item.kind));
    const unit = el('select', {}, options(UNITS, item.unit));
    const archived = el('input', {type: 'checkbox', checked: item.archived});
    const group = el('select', {}, groupOptions(groups, item.group_id, '— без группы —'));
    const vat = el('select', {},
        el('option', {value: '', selected: item.vat_rate === null}, 'без НДС'),
        VAT_RATES.map(r => el('option', {value: r, selected: item.vat_rate === r}, r + '%')));

    const save = async () => {
        if (await attempt(() => api('PUT', `items/${id}`, {
            name: name.value, kind: kind.value, unit: unit.value, archived: archived.checked, group_id: selectValue(group),
            vat_rate: selectValue(vat),
        }), 'Сохранено')) {
            route();
        }
    };
    const remove = async () => {
        if (!confirm(`Удалить «${item.name}»?`)) {
            return;
        }
        if (await attempt(() => api('DELETE', `items/${id}`), 'Удалено')) {
            location.hash = '#/items';
        }
    };

    render(view,
        el('a', {class: 'back', href: '#/items'}, '← Номенклатура'),
        el('h2', {}, item.name),
        el('div', {class: 'fields'},
            el('label', {}, 'Название', name),
            el('label', {}, 'Вид', kind),
            el('label', {}, 'Единица', unit),
            el('label', {}, 'Группа', group),
            item.kind === 'product' ? el('label', {}, 'НДС', vat) : null,
            el('label', {class: 'check'}, archived, 'в архиве'),
            el('button', {class: 'primary', onclick: save}, 'Сохранить'),
            el('button', {class: 'danger', onclick: remove}, 'Удалить')),
        photoSection(item),
        item.kind === 'raw'
            ? el('p', {class: 'muted'}, 'У сырья нет рецептуры.')
            : recipeEditor(item, recipe, all));
    if (!canManage()) {
        // кнопка удаления строки рецептуры остаётся видимой, но выключенной: таблица не съезжает
        readOnly(view, ['×']);
        view.querySelectorAll('tbody button').forEach(b => { b.disabled = true; });
    }
}

// наибольшая сторона фото после уменьшения в браузере, пикселей
const PHOTO_SIDE = 1600;

/**
 * Готовит фото к загрузке: большой снимок с телефона уменьшается до PHOTO_SIDE по большей стороне
 * и пережимается в JPEG; небольшие JPEG, PNG и WebP уходят как есть.
 */
async function preparePhoto(file) {
    let bitmap;
    try {
        bitmap = await createImageBitmap(file);
    } catch (e) {
        throw new Error(`не удалось прочитать «${file.name}»: нужна картинка JPEG, PNG или WebP`);
    }
    const scale = Math.min(1, PHOTO_SIDE / Math.max(bitmap.width, bitmap.height));
    if (scale === 1 && file.size <= 1024 * 1024 && ['image/jpeg', 'image/png', 'image/webp'].includes(file.type)) {
        return file;
    }
    const canvas = document.createElement('canvas');
    canvas.width = Math.round(bitmap.width * scale);
    canvas.height = Math.round(bitmap.height * scale);
    const ctx = canvas.getContext('2d');
    // прозрачный фон PNG в JPEG стал бы чёрным
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
    return new Promise(resolve => canvas.toBlob(resolve, 'image/jpeg', 0.85));
}

/**
 * Фото позиции: картинка (по щелчку открывается целиком), загрузка или замена и удаление.
 */
function photoSection(item) {
    const url = `/api/items/${item.id}/photo?v=${item.photo}`;
    const input = el('input', {
        type: 'file', accept: 'image/*', hidden: true,
        onchange: async () => {
            const file = input.files[0];
            if (!file) {
                return;
            }
            if (await attempt(async () => {
                const body = await preparePhoto(file);
                const res = await fetch(`/api/items/${item.id}/photo`, {
                    method: 'PUT', headers: {'Content-Type': body.type || 'application/octet-stream'}, body,
                });
                const data = await res.json().catch(() => ({}));
                if (!res.ok) {
                    throw new Error(data.error || res.status + ' ' + res.statusText);
                }
            }, 'Фото загружено')) {
                route();
            }
            input.value = '';
        },
    });
    const remove = async () => {
        if (confirm(`Удалить фото «${item.name}»?`) && await attempt(() => api('DELETE', `items/${item.id}/photo`), 'Фото удалено')) {
            route();
        }
    };
    return el('div', {class: 'photo'},
        item.photo
            ? el('a', {href: url, target: '_blank', title: 'Открыть целиком'}, el('img', {src: url, alt: item.name}))
            : el('div', {class: 'photo-empty muted'}, 'Фото нет'),
        el('div', {class: 'toolbar'},
            input,
            el('button', {onclick: () => input.click()}, item.photo ? 'Заменить фото' : 'Загрузить фото'),
            item.photo ? el('button', {class: 'danger', onclick: remove}, 'Удалить фото') : null));
}

/**
 * Редактор рецептуры: выход и строки компонентов; сохраняется целиком.
 */
function recipeEditor(item, recipe, all) {
    const used = new Set((recipe ? recipe.lines : []).map(l => l.component_id));
    // в выбор попадают все, кроме самой позиции; архивные только если уже есть в рецептуре
    const candidates = all.filter(i => i.id !== item.id && (!i.archived || used.has(i.id)));
    const byId = new Map(all.map(i => [i.id, i]));

    const output = el('input', {class: 'qty', value: recipe ? formatQty(recipe.output_qty) : '1'});
    const tbody = el('tbody');

    const componentSelect = selected => {
        const select = el('select', {}, el('option', {value: ''}, '— выберите —'));
        for (const [k, title] of Object.entries(KINDS)) {
            const group = el('optgroup', {label: title},
                candidates.filter(i => i.kind === k).map(i =>
                    el('option', {value: i.id, selected: i.id === selected}, i.name + (i.archived ? ' (архив)' : ''))));
            if (group.children.length) {
                select.append(group);
            }
        }
        return select;
    };

    const addRow = line => {
        const select = componentSelect(line ? line.component_id : null);
        const unitCell = el('td');
        const updateUnit = () => {
            const c = byId.get(Number(select.value));
            unitCell.textContent = c ? UNITS[c.unit] : '';
        };
        select.addEventListener('change', updateUnit);
        updateUnit();
        const row = el('tr', {},
            el('td', {}, select),
            el('td', {class: 'num'}, el('input', {class: 'qty', value: line ? formatQty(line.qty) : ''})),
            unitCell,
            el('td', {}, el('button', {class: 'danger', title: 'Убрать строку', onclick: () => row.remove()}, '✕')));
        tbody.append(row);
    };
    (recipe ? recipe.lines : []).forEach(addRow);
    if (!recipe) {
        addRow(null);
    }

    const save = async () => {
        const outputQty = parseQty(output.value);
        if (outputQty === null) {
            showMessage('Выход: введите число, не более трёх знаков после запятой');
            return;
        }
        const lines = [];
        for (const row of tbody.children) {
            const componentId = Number(row.querySelector('select').value);
            const qtyText = row.querySelector('input').value;
            if (!componentId && !qtyText.trim()) {
                continue;
            }
            if (!componentId) {
                showMessage('В строке рецептуры не выбран компонент');
                return;
            }
            const qty = parseQty(qtyText);
            if (qty === null) {
                showMessage(`«${byId.get(componentId).name}»: введите количество, не более трёх знаков после запятой`);
                return;
            }
            lines.push({component_id: componentId, qty});
        }
        if (await attempt(() => api('PUT', `items/${item.id}/recipe`, {output_qty: outputQty, lines}), 'Рецептура сохранена')) {
            route();
        }
    };
    const remove = async () => {
        if (confirm(`Удалить рецептуру «${item.name}»?`) && await attempt(() => api('DELETE', `items/${item.id}/recipe`), 'Рецептура удалена')) {
            route();
        }
    };

    return el('section', {},
        el('div', {class: 'toolbar section-head'},
            el('h3', {}, 'Рецептура'),
            el('button', {class: 'primary', onclick: save}, 'Сохранить рецептуру'),
            recipe ? el('button', {class: 'danger', onclick: remove}, 'Удалить рецептуру') : null),
        el('div', {class: 'fields'},
            el('label', {}, 'Выход', el('span', {}, output, ' ', UNITS[item.unit]))),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Компонент'), el('th', {class: 'num'}, 'Количество'), el('th', {}, 'Ед.'), el('th'))),
            tbody)),
        el('div', {class: 'toolbar', style: 'margin-top: 8px'},
            el('button', {onclick: () => addRow(null)}, '+ строка')));
}

// ---------- даты ----------

/** Дата в формате YYYY-MM-DD по местному времени. */
function isoDate(d) {
    const pad = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

function shiftDate(iso, days) {
    const [y, m, d] = iso.split('-').map(Number);
    return isoDate(new Date(y, m - 1, d + days));
}

/** "2026-10-12" -> "12.10.2026, пн". */
function humanDate(iso) {
    const [y, m, d] = iso.split('-').map(Number);
    const wd = ['вс', 'пн', 'вт', 'ср', 'чт', 'пт', 'сб'][new Date(y, m - 1, d).getDay()];
    return `${String(d).padStart(2, '0')}.${String(m).padStart(2, '0')}.${y}, ${wd}`;
}

/** «От кого» заявки: клиент из справочника, иначе комментарий. */
function orderFrom(o) {
    return [o.partner_name, o.comment].filter(Boolean).join(', ');
}

function lineSummary(lines) {
    return lines.map(l => `${l.name} ${formatQty(l.qty)} ${UNITS[l.unit]}`).join(', ');
}

// ---------- заявки ----------

async function renderOrders(view, params) {
    const date = params.get('date') || isoDate(new Date());
    const orders = await api('GET', `docs?type=order&from=${date}&to=${date}`);
    const go = d => { location.hash = '#/orders?date=' + d; };

    const picker = el('input', {type: 'date', value: date, onchange: e => e.target.value && go(e.target.value)});
    const checks = new Map();
    const rows = orders.map(doc => {
        const check = el('input', {type: 'checkbox', checked: true, onclick: e => e.stopPropagation()});
        checks.set(doc.id, check);
        return el('tr', {class: 'link', onclick: () => { location.hash = '#/orders/' + doc.id; }},
            el('td', {}, check),
            el('td', {class: 'num'}, doc.number),
            el('td', {}, orderFrom(doc) || el('span', {class: 'muted'}, 'не указано')),
            el('td', {}, lineSummary(doc.lines)));
    });

    const calculate = () => {
        const ids = [...checks].filter(([, c]) => c.checked).map(([id]) => id);
        if (!ids.length) {
            showMessage('Отметьте хотя бы одну заявку');
            return;
        }
        location.hash = `#/requirements?date=${date}&docs=${ids.join(',')}`;
    };

    render(view,
        el('h2', {}, 'Заявки'),
        el('div', {class: 'toolbar'},
            el('button', {title: 'Предыдущий день', onclick: () => go(shiftDate(date, -1))}, '←'),
            picker,
            el('button', {title: 'Следующий день', onclick: () => go(shiftDate(date, 1))}, '→'),
            el('span', {class: 'muted'}, humanDate(date)),
            el('button', {class: 'primary', onclick: () => { location.hash = '#/orders/new?date=' + date; }}, 'Новая заявка')),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th'), el('th', {class: 'num'}, '№'), el('th', {}, 'От кого'), el('th', {}, 'Состав'))),
            el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', {colspan: 4, class: 'muted'}, 'На эту дату заявок нет'))))),
        rows.length ? el('div', {class: 'toolbar', style: 'margin-top: 12px'},
            el('button', {class: 'primary', onclick: calculate}, 'Рассчитать потребность')) : null);
}

/** Настройки карточки по виду документа. */
// последний открытый список документов с датами и видами: туда ведёт «← Документы»
let docsListHash = '#/docs';

const DOC_TYPES = {
    order: {
        title: 'Заявка', create: 'Новая заявка', comment: 'Комментарий', partner: 'От кого',
        back: d => '#/orders?date=' + d, backTitle: '← Заявки', posting: false, fill: false, time: false,
        columns: {qty: true},
    },
    production: {
        title: 'Поступление из производства', create: 'Новое поступление', comment: 'Комментарий',
        back: () => docsListHash, backTitle: '← Документы', posting: true, fill: true,
        columns: {qty: true, cost: true},
    },
    sale: {
        title: 'Реализация', create: 'Новая реализация', comment: 'Комментарий', partner: 'Клиент',
        back: () => docsListHash, backTitle: '← Документы', posting: true, fill: true, priceType: true, typedPrices: true,
        columns: {qty: true, price: true, cost: true},
    },
    return: {
        title: 'Возврат от покупателя', create: 'Новый возврат', comment: 'Комментарий',
        back: () => docsListHash, backTitle: '← Документы', posting: true, fill: false, returnOf: true,
        columns: {qty: true, remaining: true, price: true, cost: true},
    },
    writeoff: {
        title: 'Списание', create: 'Новое списание', comment: 'Причина',
        back: () => docsListHash, backTitle: '← Документы', posting: true, fill: false,
        columns: {qty: true, cost: true},
    },
    inventory: {
        title: 'Инвентаризация', create: 'Новая инвентаризация', comment: 'Комментарий',
        back: () => docsListHash, backTitle: '← Документы', posting: true, fill: false, inventory: true,
        columns: {qty: true, book: true, cost: true},
    },
    prices: {
        title: 'Установка цен', create: 'Новая установка цен', comment: 'Комментарий',
        back: () => '#/prices', backTitle: '← Цены', posting: true, fill: false, priceType: true,
        columns: {price: true},
    },
};

/** Сумма строки в копейках: количество (тысячные) × цена за единицу, с округлением; BigInt против потери точности. */
function lineSum(qty, priceKop) {
    return Number((BigInt(qty) * BigInt(priceKop) + 500n) / 1000n);
}

/**
 * Карточка документа; id = null для нового (вид берётся из params.type, по умолчанию заявка).
 * Колонки зависят от вида: количество, цена (в реализации - из типа цен, можно поправить),
 * сумма, себестоимость на дату документа и её сумма.
 */
async function renderDoc(view, id, params) {
    const [doc, products, priceTypes, partners, groups, period] = await Promise.all([
        id ? api('GET', `docs/${id}`) : Promise.resolve(null),
        api('GET', 'items?kind=product&archived=1'),
        api('GET', 'price-types'),
        api('GET', 'partners?archived=1'),
        api('GET', 'groups'),
        api('GET', 'period'),
    ]);
    const type = doc ? doc.type : (params.get('type') || 'order');
    if (type === 'prices' && !canManage()) {
        throw new Error('недостаточно прав: установки цен ведёт и смотрит технолог');
    }
    const cfg = DOC_TYPES[type];
    if (!cfg) {
        throw new Error('неизвестный вид документа: ' + type);
    }
    const cols = cfg.columns;
    const posted = doc && doc.status === 'posted';
    // документ в закрытом периоде только для просмотра и печати
    const locked = Boolean(doc && period.closed_till && doc.date <= period.closed_till);

    const editable = !locked;
    const inDoc = new Set((doc ? doc.lines : []).map(l => l.item_id));
    const candidates = products.filter(p => !p.archived || inDoc.has(p.id));
    const byId = new Map(products.map(p => [p.id, p]));
    const date = doc ? doc.date : (params.get('date') || isoDate(new Date()));

    // возврат: реализация-основание и сколько по ней можно вернуть (без учёта этого возврата)
    const baseId = doc ? doc.base_id : (params.get('base') ? Number(params.get('base')) : null);
    let base = null;
    let returnable = new Map();
    if (cfg.returnOf) {
        if (!baseId) {
            throw new Error('возврат оформляется из проведённой реализации: кнопка «Оформить возврат» в ней');
        }
        const [b, r] = await Promise.all([
            api('GET', `docs/${baseId}`),
            api('GET', `docs/${baseId}/returnable` + (doc ? `?exclude=${doc.id}` : '')),
        ]);
        base = b;
        returnable = new Map(r.map(x => [x.item_id, x]));
    }
    // реализация: проведённые и черновые возвраты по ней
    const returns = type === 'sale' && doc
        ? (await api('GET', `docs?type=return&from=${doc.date}`)).filter(r => r.base_id === doc.id) : [];

    // новый документ - сейчас; новый возврат - не раньше реализации
    const today = isoDate(new Date());
    const now = new Date().toTimeString().slice(0, 5);
    let initDate = doc ? doc.date : date;
    let initTime = doc ? doc.time : now;
    if (!doc && cfg.returnOf) {
        initDate = today;
        if (today < base.date || (today === base.date && now < base.time)) {
            initDate = base.date;
            initTime = base.time;
        }
    }
    const dateInput = el('input', {type: 'date', value: initDate});
    const timeInput = el('input', {type: 'time', value: initTime});
    const comment = el('input', {value: doc ? doc.comment : '', size: 40});
    const chosenType = doc ? doc.price_type_id : (params.get('price_type') ? Number(params.get('price_type')) : null);
    const priceType = el('select', {},
        el('option', {value: '', selected: !chosenType}, '— выберите —'),
        priceTypes.map(t => el('option', {value: t.id, selected: t.id === chosenType}, t.name)));
    const partnerId = doc ? doc.partner_id : null;
    const partner = el('select', {},
        el('option', {value: '', selected: !partnerId}, '— не выбран —'),
        partners.filter(p => !p.archived || p.id === partnerId).map(p =>
            el('option', {value: p.id, selected: p.id === partnerId}, p.name + (p.archived ? ' (архив)' : ''))));
    const tbody = el('tbody');
    const totals = el('div', {class: 'totals'});

    // инвентаризация: область (группа с подгруппами) и учётные остатки на момент документа без его движений
    const scope = el('select', {}, groupOptions(groups, doc ? doc.group_id : null, 'вся продукция'));
    let book = new Map();
    const inScope = itemId => {
        const g = selectValue(scope);
        return !g || groupWithDescendants(groups, g).has((byId.get(itemId) || {}).group_id);
    };
    const loadBook = async () => {
        if (!cfg.inventory || !dateInput.value) {
            return;
        }
        const q = `stock?date=${dateInput.value}&time=${timeInput.value || '23:59'}` + (scope.value ? `&group=${scope.value}` : '') + (doc ? `&exclude=${doc.id}` : '');
        book = new Map((await api('GET', q)).map(b => [b.item_id, b]));
    };
    const zeroed = el('div', {class: 'warnings', hidden: true});

    // себестоимость и типовые цены на дату документа; у сохранённых строк себестоимость своя (записана сервером)
    let costs = new Map();
    let typed = new Map();
    const loadPrices = async () => {
        const d = dateInput.value;
        const t = cfg.time === false || !timeInput.value ? '' : `&time=${timeInput.value}`;
        if (cols.cost && d) {
            costs = new Map((await api('GET', `prices?type=1&date=${d}${t}`)).map(r => [r.item_id, r.price_kop]));
        }
        if (cfg.typedPrices && d && priceType.value) {
            typed = new Map((await api('GET', `prices?type=${priceType.value}&date=${d}${t}`)).map(r => [r.item_id, r.price_kop]));
        } else {
            typed = new Map();
        }
    };
    await loadPrices();
    await loadBook();

    const numberOrNull = text => (text === '' || text === undefined ? null : Number(text));

    /** Пересчёт сумм строк, отметок цены и итогов. */
    const update = () => {
        let sum = 0;
        let costSum = 0;
        let missingCost = false;
        let missingPrice = 0;
        for (const row of tbody.children) {
            const itemId = Number(row.querySelector('select').value);
            const qty = cols.qty ? parseQty(row.querySelector('input.qty').value) : null;
            if (cols.price) {
                const input = row.querySelector('input.money');
                const price = parseMoney(input.value);
                const typical = typed.get(itemId);
                input.classList.toggle('missing', !!itemId && cfg.typedPrices && price === null);
                input.classList.toggle('manual', cfg.typedPrices && price !== null && typical !== undefined && price !== typical);
                input.title = !cfg.typedPrices ? '' : typical === undefined ? 'нет цены в выбранном типе цен' : 'типовая цена ' + formatMoney(typical);
                if (itemId && cfg.typedPrices && price === null) {
                    missingPrice++;
                }
                const lineTotal = qty !== null && price !== null && cols.qty ? lineSum(qty, price) : null;
                const cell = row.querySelector('td.sum');
                if (cell) {
                    cell.textContent = lineTotal === null ? '' : formatMoney(lineTotal);
                }
                sum += lineTotal || 0;
            }
            if (cols.cost) {
                const cost = numberOrNull(row.dataset.cost);
                if (itemId && cost === null) {
                    missingCost = true;
                }
                const lineCost = qty !== null && cost !== null ? lineSum(qty, cost) : null;
                row.querySelector('td.cost-sum').textContent = lineCost === null ? '' : formatMoney(lineCost);
                costSum += lineCost || 0;
            }
        }
        if (cfg.inventory) {
            const listed = new Set();
            for (const row of tbody.children) {
                const itemId = Number(row.querySelector('select').value);
                listed.add(itemId);
                const fact = parseQty(row.querySelector('input.qty').value);
                const b = book.get(itemId);
                const bookQty = b ? b.qty : 0;
                row.querySelector('td.book').textContent = itemId ? formatQty(bookQty) : '';
                const diffCell = row.querySelector('td.diff');
                const diff = itemId && fact !== null ? fact - bookQty : null;
                diffCell.textContent = diff === null || diff === 0 ? '' : (diff > 0 ? '+' : '') + formatQty(diff);
                diffCell.className = 'num diff ' + (diff > 0 ? 'plus' : diff < 0 ? 'minus' : '');
            }
            const lost = [...book.values()].filter(b => !listed.has(b.item_id));
            zeroed.hidden = !lost.length;
            zeroed.textContent = lost.length
                ? 'При проведении будут обнулены (нет в документе): ' + lost.map(b => `${b.name} ${formatQty(b.qty)} ${UNITS[b.unit]}`).join(', ')
                : '';
        }
        const parts = [];
        if (cols.price && cols.qty) {
            parts.push(el('span', {}, 'Сумма: ', el('strong', {}, formatMoney(sum) + ' ₽')));
        }
        if (cols.cost) {
            parts.push(el('span', {title: missingCost ? 'у части строк нет себестоимости' : ''},
                'Себестоимость: ', el('strong', {}, formatMoney(costSum) + ' ₽' + (missingCost ? ' *' : ''))));
        }
        if (cfg.typedPrices && cols.cost && costSum > 0 && !missingCost) {
            const margin = sum - costSum;
            parts.push(el('span', {}, 'Наценка: ', el('strong', {}, `${formatMoney(margin)} ₽ (${Math.round(margin * 100 / costSum)}%)`)));
        } else if (cfg.typedPrices && cols.cost && missingCost) {
            parts.push(el('span', {class: 'muted'}, 'наценка не считается: у части строк нет себестоимости'));
        }
        if (missingPrice) {
            parts.push(el('span', {class: 'error-text'}, `без цены строк: ${missingPrice}, провести нельзя`));
        }
        totals.replaceChildren(...parts);
    };

    const addRow = line => {
        const select = el('select', {}, el('option', {value: ''}, '— выберите —'),
            candidates.map(p => el('option', {value: p.id, selected: line && p.id === line.item_id}, p.name + (p.archived ? ' (архив)' : ''))));
        const unitCell = el('td');
        const costCell = el('td', {class: 'num'});
        const priceInput = el('input', {class: 'money', value: line && line.price_kop !== null && line.price_kop !== undefined ? formatMoney(line.price_kop) : '', oninput: update, readonly: !!cfg.returnOf});
        if (cfg.returnOf) {
            select.disabled = true;
        }
        const left = line && returnable.get(line.item_id);
        const row = el('tr');
        const setCost = cost => {
            row.dataset.cost = cost === null || cost === undefined ? '' : cost;
            costCell.replaceChildren(cost === null || cost === undefined ? el('span', {class: 'muted', title: 'себестоимость не установлена'}, 'нет') : formatMoney(cost));
        };
        const onItem = initial => {
            const p = byId.get(Number(select.value));
            unitCell.textContent = p ? (cols.price && !cols.qty ? 'за ' : '') + UNITS[p.unit] : '';
            if (cols.cost) {
                setCost(initial && line && line.cost_kop !== undefined ? line.cost_kop : (p ? costs.get(p.id) : null));
            }
            if (cfg.typedPrices && !initial) {
                const price = p ? typed.get(p.id) : undefined;
                priceInput.value = price === undefined ? '' : formatMoney(price);
            }
            update();
        };
        select.addEventListener('change', () => onItem(false));
        if (cfg.inventory) {
            // в выбор - только позиции области
            for (const option of [...select.options].slice(1)) {
                option.hidden = !inScope(Number(option.value));
            }
        }
        row.append(
            el('td', {}, select),
            cols.qty ? el('td', {class: 'num'}, el('input', {class: 'qty', value: line && line.qty ? formatQty(line.qty) : '', oninput: update})) : '',
            cols.remaining ? el('td', {class: 'num muted'}, left ? formatQty(left.sold - left.returned) : '') : '',
            cols.book ? el('td', {class: 'num muted book'}) : '',
            cols.book ? el('td', {class: 'num diff'}) : '',
            unitCell,
            cols.price ? el('td', {class: 'num'}, priceInput) : '',
            cols.price && cols.qty ? el('td', {class: 'num sum'}) : '',
            cols.cost ? costCell : '',
            cols.cost ? el('td', {class: 'num cost-sum'}) : '',
            el('td', {}, el('button', {class: 'danger', title: 'Убрать строку', onclick: () => { row.remove(); update(); }}, '✕')));
        tbody.append(row);
        onItem(!!line && line.price_kop !== undefined);
        return row;
    };
    if (doc) {
        doc.lines.forEach(addRow);
    } else if (cfg.returnOf) {
        // новый возврат: всё, что ещё не возвращено, по ценам и себестоимости реализации
        [...returnable.values()].filter(r => r.sold > r.returned).forEach(r =>
            addRow({item_id: r.item_id, qty: r.sold - r.returned, price_kop: r.price_kop}));
    } else {
        addRow(null);
    }

    /** После смены даты или типа цен: новая себестоимость, по желанию - цены нового типа во всех строках. */
    const reprice = async () => {
        await loadPrices();
        await loadBook();
        const refill = cfg.typedPrices && tbody.querySelector('select') && confirm('Подставить цены на эту дату и этого типа во все строки? Поправленные вручную цены будут заменены.');
        for (const row of tbody.children) {
            const itemId = Number(row.querySelector('select').value);
            if (cols.cost) {
                const cost = costs.get(itemId);
                row.dataset.cost = cost === undefined ? '' : cost;
                row.querySelector('td.cost-sum').previousSibling.replaceChildren(cost === undefined
                    ? el('span', {class: 'muted', title: 'себестоимость не установлена'}, 'нет') : formatMoney(cost));
            }
            if (refill && itemId) {
                const price = typed.get(itemId);
                row.querySelector('input.money').value = price === undefined ? '' : formatMoney(price);
            }
        }
        update();
    };
    dateInput.addEventListener('change', () => dateInput.value && reprice().catch(err => showMessage(err.message)));
    timeInput.addEventListener('change', () => timeInput.value && reprice().catch(err => showMessage(err.message)));
    priceType.addEventListener('change', () => reprice().catch(err => showMessage(err.message)));
    scope.addEventListener('change', () => {
        for (const row of tbody.children) {
            for (const option of [...row.querySelector('select').options].slice(1)) {
                option.hidden = !inScope(Number(option.value));
            }
        }
        loadBook().then(update).catch(err => showMessage(err.message));
    });

    /** Строки по учётным остаткам области: факт = учёт, дальше правится насчитанное. */
    const fillBook = async () => {
        const filled = [...tbody.children].some(r => r.querySelector('select').value);
        if (filled && !confirm('Заменить строки учётными остатками?')) {
            return;
        }
        await loadBook();
        tbody.replaceChildren();
        [...book.values()].forEach(b => addRow({item_id: b.item_id, qty: b.qty}));
        if (!book.size) {
            addRow(null);
        }
        update();
        showMessage(`Заполнено учётными остатками: ${book.size}. Исправьте факт там, где насчитали иначе`, 'ok');
    };

    /** Строки из таблицы; null и сообщение, если что-то не так. */
    const collect = () => {
        const lines = [];
        for (const row of tbody.children) {
            const itemId = Number(row.querySelector('select').value);
            const qtyText = cols.qty ? row.querySelector('input.qty').value : '';
            const priceText = cols.price ? row.querySelector('input.money').value : '';
            if (!itemId && !qtyText.trim() && !priceText.trim()) {
                continue;
            }
            if (!itemId) {
                showMessage('В строке не выбрана продукция');
                return null;
            }
            const line = {item_id: itemId};
            if (cols.qty) {
                line.qty = parseQty(qtyText);
                if (line.qty === null) {
                    showMessage(`«${byId.get(itemId).name}»: введите количество, не более трёх знаков после запятой`);
                    return null;
                }
            }
            if (cols.price) {
                line.price_kop = parseMoney(priceText);
                if (line.price_kop === null && (!cfg.typedPrices || priceText.trim())) {
                    showMessage(`«${byId.get(itemId).name}»: введите цену в рублях, не более двух знаков после запятой`);
                    return null;
                }
            }
            lines.push(line);
        }
        return lines;
    };

    const save = async andPost => {
        const lines = collect();
        if (!lines) {
            return;
        }
        const body = {type, date: dateInput.value, comment: comment.value, lines};
        if (cfg.time !== false) {
            body.time = timeInput.value || '00:00';
        }
        if (cfg.priceType) {
            body.price_type_id = selectValue(priceType);
        }
        if (cfg.partner) {
            body.partner_id = selectValue(partner);
        }
        if (cfg.returnOf) {
            body.base_id = baseId;
        }
        if (cfg.inventory) {
            body.group_id = selectValue(scope);
        }
        let saved;
        const ok = await attempt(async () => {
            saved = await api(doc ? 'PUT' : 'POST', doc ? `docs/${doc.id}` : 'docs', body);
            if (andPost) {
                saved = await api('POST', `docs/${saved.id}/post`);
            }
        }, andPost ? 'Сохранено и проведено' : posted ? 'Сохранено и перепроведено' : 'Сохранено');
        if (saved && !doc && cfg.posting) {
            // документ создан: дальше работаем с ним, даже если проведение не удалось
            history.replaceState(null, '', '#/docs/' + saved.id);
        }
        if (ok && !cfg.posting) {
            location.hash = cfg.back(saved.date);
        } else if (saved) {
            const message = document.getElementById('message').textContent;
            const kind = document.getElementById('message').className;
            await renderDoc(view, saved.id, params);
            showMessage(message, kind);
        }
    };
    const unpost = async () => {
        if (await attempt(() => api('POST', `docs/${doc.id}/unpost`), 'Проведение отменено')) {
            await renderDoc(view, doc.id, params);
        }
    };
    const remove = async () => {
        const name = `${cfg.title.toLowerCase()} № ${doc.number}`;
        if (confirm(`Удалить ${name}?`) && await attempt(() => api('DELETE', `docs/${doc.id}`), 'Удалено')) {
            location.hash = cfg.back(doc.date);
        }
    };

    /** Панель «Заполнить по заявкам»: заявки на дату с галочками, строки заменяются суммой. */
    const fillPanel = el('div', {class: 'panel', hidden: true});
    const openFill = async (fillDate) => {
        const orders = await api('GET', `docs?type=order&from=${fillDate}&to=${fillDate}`);
        const checks = orders.map(o => [o, el('input', {type: 'checkbox', checked: true})]);
        const datePick = el('input', {type: 'date', value: fillDate, onchange: e => e.target.value && openFill(e.target.value).catch(err => showMessage(err.message))});
        const apply = async () => {
            const chosen = checks.filter(([, c]) => c.checked).map(([o]) => o);
            if (!chosen.length) {
                showMessage('Отметьте хотя бы одну заявку');
                return;
            }
            const filled = [...tbody.children].some(r => r.querySelector('select').value);
            if (filled && !confirm('Заменить строки документа суммой выбранных заявок?')) {
                return;
            }
            const req = await api('GET', 'requirements?docs=' + chosen.map(o => o.id).join(','));
            tbody.replaceChildren();
            req.products.forEach(p => {
                const row = addRow({item_id: p.item_id, qty: p.qty});
                row.querySelector('select').dispatchEvent(new Event('change'));
            });
            const clients = new Set(chosen.map(o => o.partner_id));
            if (cfg.partner && !partner.value && clients.size === 1 && chosen[0].partner_id) {
                partner.value = chosen[0].partner_id;
            }
            if (!comment.value.trim()) {
                comment.value = `по заявкам № ${chosen.map(o => o.number).join(', ')} на ${humanDate(fillDate).split(',')[0]}`;
            }
            fillPanel.hidden = true;
            update();
            showMessage('Строки заполнены по заявкам, проверьте количества', 'ok');
        };
        fillPanel.replaceChildren(
            el('div', {class: 'toolbar'}, el('strong', {}, 'Заявки на'), datePick,
                el('button', {onclick: () => { fillPanel.hidden = true; }}, 'Закрыть')),
            orders.length
                ? el('div', {}, checks.map(([o, c]) => el('label', {class: 'check'}, c,
                    `№ ${o.number}${orderFrom(o) ? ' (' + orderFrom(o) + ')' : ''}: ${lineSummary(o.lines)}`)))
                : el('p', {class: 'muted'}, 'На эту дату заявок нет'),
            orders.length ? el('div', {class: 'toolbar', style: 'margin-top: 8px'},
                el('button', {class: 'primary', onclick: () => apply().catch(err => showMessage(err.message))}, 'Заполнить')) : null);
        fillPanel.hidden = false;
    };

    const title = doc ? `${cfg.title} № ${doc.number}` : cfg.create;
    render(view,
        el('a', {class: 'back', href: cfg.back(date)}, cfg.backTitle),
        // заголовок и кнопки прилипают к верху окна, чтобы не листать длинный документ
        el('div', {class: 'actions'},
            el('h2', {}, title, cfg.posting && doc ? el('span', {class: 'status ' + doc.status}, posted ? 'проведён' : 'не проведён') : null),
            el('div', {class: 'toolbar'},
                !editable ? null : posted
                    ? el('button', {class: 'primary', onclick: () => save(false)}, 'Сохранить и перепровести')
                    : el('button', {class: cfg.posting ? null : 'primary', onclick: () => save(false)}, 'Сохранить'),
                editable && cfg.posting && !posted ? el('button', {class: 'primary', onclick: () => save(true)}, 'Сохранить и провести') : null,
                editable && posted ? el('button', {onclick: unpost}, 'Отменить проведение') : null,
                editable && cfg.fill ? el('button', {onclick: () => openFill(dateInput.value || date).catch(err => showMessage(err.message))}, 'Заполнить по заявкам') : null,
                type === 'sale' && posted ? el('button', {onclick: () => { location.hash = `#/docs/new?type=return&base=${doc.id}`; }}, 'Оформить возврат') : null,
                editable && cfg.inventory ? el('button', {onclick: () => fillBook().catch(err => showMessage(err.message))}, 'Заполнить учётными остатками') : null,
                doc ? el('button', {onclick: () => window.open(`#/print/${doc.id}`, '_blank')}, 'Печать') : null,
                doc && type === 'sale' ? el('button', {onclick: () => window.open(`#/print/${doc.id}?form=torg12`, '_blank')}, 'ТОРГ-12') : null,
                doc && type === 'sale' ? el('button', {onclick: () => window.open(`#/print/${doc.id}?form=upd`, '_blank')}, 'УПД') : null,
                editable && doc && !posted ? el('button', {class: 'danger', onclick: remove}, 'Удалить') : null)),
        locked ? el('p', {class: 'warnings'}, `Период по ${humanDate(period.closed_till).split(',')[0]} закрыт: документ только для просмотра. `,
            el('a', {href: '#/settings'}, 'Закрытие периода')) : null,
        doc && doc.created_by ? el('p', {class: 'muted authors'}, `Создал: ${doc.created_by}, ${humanMoment(doc.created_at)}`,
            doc.updated_by && doc.updated_at !== doc.created_at ? `; изменил: ${doc.updated_by}, ${humanMoment(doc.updated_at)}` : '') : null,
        base ? el('p', {},
            'По ', el('a', {href: '#/docs/' + base.id}, `реализации № ${base.number} от ${humanDate(base.date).split(',')[0]}`),
            base.partner_name ? `, клиент ${base.partner_name}` : '') : null,
        returns.length ? el('p', {}, 'Возвраты: ', returns.map((r, i) => [i ? ', ' : '',
            el('a', {href: '#/docs/' + r.id}, `№ ${r.number}`), r.status === 'posted' ? '' : ' (не проведён)'])) : null,
        el('div', {class: 'fields'},
            el('label', {}, 'Дата', dateInput),
            cfg.time === false ? null : el('label', {}, 'Время', timeInput),
            cfg.partner ? el('label', {}, cfg.partner, partner) : null,
            cfg.priceType && !cfg.returnOf ? el('label', {}, 'Тип цен', priceType) : null,
            cfg.inventory ? el('label', {}, 'Область', scope) : null,
            el('label', {}, cfg.comment, comment)),
        fillPanel,
        cfg.inventory ? zeroed : null,
        el('div', {class: 'table-wrap', style: 'margin-top: 12px'}, el('table', {},
            el('thead', {}, el('tr', {},
                el('th', {}, 'Продукция'),
                cols.qty ? el('th', {class: 'num'}, cfg.returnOf ? 'Вернуть' : cfg.inventory ? 'Факт' : 'Количество') : null,
                cols.remaining ? el('th', {class: 'num'}, 'Можно вернуть') : null,
                cols.book ? el('th', {class: 'num'}, 'Учёт') : null,
                cols.book ? el('th', {class: 'num'}, 'Разница') : null,
                el('th', {}, 'Ед.'),
                cols.price ? el('th', {class: 'num'}, 'Цена, ₽') : null,
                cols.price && cols.qty ? el('th', {class: 'num'}, 'Сумма, ₽') : null,
                cols.cost ? el('th', {class: 'num'}, 'Себест., ₽') : null,
                cols.cost ? el('th', {class: 'num'}, 'Сумма себест., ₽') : null,
                el('th'))),
            tbody)),
        cfg.returnOf || locked ? null : el('div', {class: 'toolbar', style: 'margin-top: 8px'},
            el('button', {onclick: () => addRow(null).querySelector('select').focus()}, '+ строка')),
        totals);
    update();
    if (locked) {
        view.querySelectorAll('.fields input, .fields select, tbody input, tbody select, tbody button').forEach(e => { e.disabled = true; });
    }
}

// ---------- документы ----------

// виды в списке документов, порядок кнопок фильтра; заявки живут в своём разделе
const DOC_FILTER = {production: 'Поступление', sale: 'Реализация', return: 'Возврат', writeoff: 'Списание', inventory: 'Инвентаризация', prices: 'Установка цен'};
const ALL_DOC_TYPES = Object.keys(DOC_FILTER);

async function renderDocs(view, params) {
    // установки цен видит только технолог и администратор
    const listTypes = ALL_DOC_TYPES.filter(t => t !== 'prices' || canManage());
    // по умолчанию неделя до и после сегодня: документы, оформленные заранее, тоже видны
    const today = isoDate(new Date());
    const from = params.get('from') || shiftDate(today, -7);
    const to = params.get('to') || shiftDate(today, 7);
    // выбранные виды; ни одного - все виды
    const types = (params.get('types') || '').split(',').filter(t => listTypes.includes(t));
    docsListHash = location.hash;
    const docs = await api('GET', `docs?type=${(types.length ? types : listTypes).join(',')}&from=${from}&to=${to}`);
    const go = (f, t, ty) => { location.hash = `#/docs?from=${f}&to=${t}` + (ty.length ? '&types=' + ty.join(',') : ''); };
    const fromInput = el('input', {type: 'date', value: from, onchange: e => e.target.value && go(e.target.value, to, types)});
    const toInput = el('input', {type: 'date', value: to, onchange: e => e.target.value && go(from, e.target.value, types)});
    const toggle = type => go(from, to, listTypes.filter(t => (t === type) !== types.includes(t)));
    const typeButtons = el('div', {class: 'tabs toolbar'}, 'вид',
        el('button', {class: types.length ? null : 'active', onclick: () => go(from, to, [])}, 'Все'),
        listTypes.map(t => el('button', {class: types.includes(t) ? 'active' : null, onclick: () => toggle(t)}, DOC_FILTER[t])));

    const rows = docs.slice().reverse().map(doc => el('tr', {class: 'link', onclick: () => { location.hash = '#/docs/' + doc.id; }},
        el('td', {class: 'nowrap'}, humanDate(doc.date).split(',')[0] + ' ' + doc.time),
        el('td', {}, `${DOC_TYPES[doc.type].title} № ${doc.number}`),
        el('td', {class: 'nowrap'}, el('span', {class: 'status ' + doc.status}, doc.status === 'posted' ? 'проведён' : 'не проведён')),
        el('td', {}, orderFrom(doc)),
        el('td', {}, doc.type === 'prices' ? doc.lines.map(l => l.name).join(', ') : lineSummary(doc.lines))));

    render(view,
        el('h2', {}, 'Документы'),
        el('div', {class: 'toolbar'},
            el('button', {class: 'primary', onclick: () => { location.hash = '#/docs/new?type=production'; }}, 'Новое поступление'),
            el('button', {class: 'primary', onclick: () => { location.hash = '#/docs/new?type=sale'; }}, 'Новая реализация'),
            el('button', {onclick: () => { location.hash = '#/docs/new?type=writeoff'; }}, 'Новое списание'),
            el('button', {onclick: () => { location.hash = '#/docs/new?type=inventory'; }}, 'Новая инвентаризация'),
            canManage() ? el('button', {onclick: () => { location.hash = '#/docs/new?type=prices'; }}, 'Новая установка цен') : null,
            el('button', {onclick: () => { location.hash = '#/repost'; }}, 'Перепровести документы')),
        el('div', {class: 'toolbar'}, 'с', fromInput, 'по', toInput),
        typeButtons,
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Дата и время'), el('th', {}, 'Документ'), el('th', {}, 'Статус'), el('th', {}, 'Клиент, комментарий'), el('th', {}, 'Состав'))),
            el('tbody', {}, rows.length ? rows : el('tr', {}, el('td', {colspan: 5, class: 'muted'}, types.length ? 'За эти дни документов выбранных видов нет' : 'За эти дни документов нет'))))));
}

// ---------- цены ----------

async function renderPrices(view, params) {
    if (!canManage()) {
        throw new Error('недостаточно прав: цены ведёт технолог');
    }
    const types = await api('GET', 'price-types');
    const typeId = Number(params.get('type')) || types[0].id;
    const current = types.find(t => t.id === typeId) || types[0];
    const date = params.get('date') || isoDate(new Date());
    const list = await api('GET', `prices?type=${current.id}&date=${date}`);
    const go = (t, d) => { location.hash = `#/prices?type=${t}&date=${d}`; };

    const addType = async () => {
        const name = prompt('Новый тип цен:');
        let created;
        if (name && await attempt(async () => { created = await api('POST', 'price-types', {name}); }, 'Тип цен создан')) {
            go(created.id, date);
        }
    };
    const renameType = async () => {
        const name = prompt('Название типа цен:', current.name);
        if (name && await attempt(() => api('PUT', `price-types/${current.id}`, {name}), 'Тип цен переименован')) {
            route();
        }
    };
    const removeType = async () => {
        if (confirm(`Удалить тип цен «${current.name}»?`) && await attempt(() => api('DELETE', `price-types/${current.id}`), 'Тип цен удалён')) {
            go(types[0].id, date);
        }
    };

    render(view,
        el('h2', {}, 'Цены'),
        el('div', {class: 'tabs toolbar'},
            types.map(t => el('button', {class: t.id === current.id ? 'active' : null, onclick: () => go(t.id, date)}, t.name)),
            canManage() ? el('button', {onclick: addType}, '+ тип цен') : null),
        canManage() ? el('div', {class: 'toolbar'},
            el('button', {class: 'primary', onclick: () => { location.hash = `#/docs/new?type=prices&price_type=${current.id}&date=${date}`; }}, 'Новая установка цен'),
            current.system ? null : el('button', {onclick: renameType}, 'Переименовать'),
            current.system ? null : el('button', {class: 'danger', onclick: removeType}, 'Удалить тип')) : null,
        el('div', {class: 'toolbar'},
            'Цены «' + current.name + '» на',
            el('input', {type: 'date', value: date, onchange: e => e.target.value && go(current.id, e.target.value)}),
            el('span', {class: 'muted'}, humanDate(date))),
        current.system ? el('p', {class: 'muted'}, 'Себестоимость подставляется в строки складских документов на их дату.') : null,
        el('div', {class: 'table-wrap'}, el('table', {class: 'needs'},
            el('thead', {}, el('tr', {}, el('th', {}, 'Продукция'), el('th', {class: 'num'}, 'Цена, ₽'), el('th', {}, 'Ед.'), el('th', {}, 'Действует с'))),
            el('tbody', {}, list.length
                ? list.map(r => el('tr', {class: 'link', onclick: () => { location.hash = '#/docs/' + r.doc_id; }},
                    el('td', {}, r.name),
                    el('td', {class: 'num'}, formatMoney(r.price_kop)),
                    el('td', {}, 'за ' + UNITS[r.unit]),
                    el('td', {class: 'muted'}, humanDate(r.date))))
                : el('tr', {}, el('td', {colspan: 4, class: 'muted'}, 'Цен этого типа на дату нет'))))));
}

// ---------- клиенты ----------

const partnersState = {archived: false};

async function renderPartners(view) {
    const partners = await api('GET', 'partners' + (partnersState.archived ? '?archived=1' : ''));
    const name = el('input', {placeholder: 'Название или имя', required: true, size: 28});
    const phone = el('input', {placeholder: 'Телефон', size: 16});
    const note = el('input', {placeholder: 'Примечание', size: 28});
    const form = el('form', {
        class: 'inline',
        onsubmit: async e => {
            e.preventDefault();
            if (await attempt(() => api('POST', 'partners', {name: name.value, phone: phone.value, note: note.value}), 'Клиент добавлен')) {
                route();
            }
        },
    }, name, phone, note, el('button', {class: 'primary'}, 'Добавить'));
    render(view,
        el('h2', {}, 'Клиенты'),
        form,
        el('div', {class: 'toolbar', style: 'margin-top: 12px'},
            el('label', {class: 'check'},
                el('input', {type: 'checkbox', checked: partnersState.archived, onchange: e => { partnersState.archived = e.target.checked; route(); }}),
                'показать архив')),
        el('div', {class: 'table-wrap'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Клиент'), el('th', {}, 'Телефон'), el('th', {}, 'Примечание'))),
            el('tbody', {}, partners.length
                ? partners.map(p => el('tr', {class: 'link' + (p.archived ? ' archived' : ''), onclick: () => { location.hash = '#/partners/' + p.id; }},
                    el('td', {}, p.name, p.archived ? el('span', {class: 'muted'}, ' (архив)') : null),
                    el('td', {}, p.phone),
                    el('td', {class: 'muted'}, p.note)))
                : el('tr', {}, el('td', {colspan: 3, class: 'muted'}, 'Клиентов нет'))))));
}

async function renderPartner(view, id) {
    const p = await api('GET', `partners/${id}`);
    const name = el('input', {value: p.name, size: 28});
    const phone = el('input', {value: p.phone, size: 16});
    const note = el('input', {value: p.note, size: 40});
    const inn = el('input', {value: p.inn, size: 14});
    const kpp = el('input', {value: p.kpp, size: 11});
    const address = el('input', {value: p.address, size: 60});
    const archived = el('input', {type: 'checkbox', checked: p.archived});
    const save = async () => {
        const body = {name: name.value, phone: phone.value, note: note.value, inn: inn.value, kpp: kpp.value, address: address.value, archived: archived.checked};
        if (await attempt(() => api('PUT', `partners/${id}`, body), 'Сохранено')) {
            route();
        }
    };
    const remove = async () => {
        if (confirm(`Удалить клиента «${p.name}»?`) && await attempt(() => api('DELETE', `partners/${id}`), 'Клиент удалён')) {
            location.hash = '#/partners';
        }
    };
    render(view,
        el('a', {class: 'back', href: '#/partners'}, '← Клиенты'),
        el('h2', {}, p.name),
        el('div', {class: 'fields'},
            el('label', {}, 'Название', name),
            el('label', {}, 'Телефон', phone),
            el('label', {}, 'Примечание', note)),
        el('h3', {}, 'Реквизиты для накладных'),
        el('div', {class: 'fields'},
            el('label', {}, 'ИНН', inn),
            el('label', {}, 'КПП', kpp),
            el('label', {}, 'Адрес', address)),
        el('div', {class: 'fields', style: 'margin-top: 12px'},
            el('label', {class: 'check'}, archived, 'в архиве'),
            el('button', {class: 'primary', onclick: save}, 'Сохранить'),
            el('button', {class: 'danger', onclick: remove}, 'Удалить')));
}

// ---------- организация ----------

const SETTINGS_FORM = [
    ['Организация', [['org_name', 'Название', 40], ['org_inn', 'ИНН', 14], ['org_kpp', 'КПП', 11], ['org_okpo', 'ОКПО', 12],
        ['org_address', 'Адрес', 60], ['org_phone', 'Телефон', 18]]],
    ['Банк', [['bank_name', 'Банк', 40], ['bank_bik', 'БИК', 11], ['bank_account', 'Расчётный счёт', 24], ['bank_corr_account', 'Корр. счёт', 24]]],
    ['Подписи в документах', [['director', 'Руководитель', 28], ['accountant', 'Главный бухгалтер', 28], ['storekeeper', 'Отпуск произвёл (кладовщик)', 28]]],
];

async function renderSettings(view) {
    const [settings, period] = await Promise.all([api('GET', 'settings'), api('GET', 'period')]);
    const inputs = {};
    const vat = el('input', {type: 'checkbox', checked: settings.vat_payer === '1'});
    const footer = el('input', {value: settings.doc_footer, size: 80});
    const save = async () => {
        const body = Object.fromEntries(Object.entries(inputs).map(([k, input]) => [k, input.value.trim()]));
        body.vat_payer = vat.checked ? '1' : '0';
        body.doc_footer = footer.value;
        await attempt(() => api('PUT', 'settings', body), 'Реквизиты сохранены');
    };
    render(view,
        el('div', {class: 'toolbar section-head', style: 'margin-top: 0'},
            el('h2', {}, 'Организация'),
            el('button', {class: 'primary', onclick: save}, 'Сохранить')),
        el('p', {class: 'muted'}, 'Реквизиты печатаются в накладных (ТОРГ-12, УПД) и в шапке документов.'),
        SETTINGS_FORM.map(([title, fields]) => [
            el('h3', {}, title),
            el('div', {class: 'fields'}, fields.map(([key, label, size]) => {
                inputs[key] = el('input', {value: settings[key], size});
                return el('label', {}, label, inputs[key]);
            })),
        ]),
        el('h3', {}, 'НДС'),
        el('label', {class: 'check'}, vat, 'организация - плательщик НДС (ставка указывается в карточке продукции, цены включают НДС)'),
        el('h3', {}, 'Текст внизу печатных документов'),
        el('div', {class: 'fields'}, el('label', {}, 'Например, условия возврата или контакты', footer)),
        periodSection(period));
    if (!canManage()) {
        readOnly(view);
        view.insertBefore(el('p', {class: 'warnings'}, 'Реквизиты и закрытие периода меняет технолог или администратор.'), view.children[1]);
    }
}

/**
 * Закрытие периода: документы по дату включительно нельзя создавать, менять, проводить и отменять.
 * Сдвиг даты назад открывает период и требует подтверждения.
 */
function periodSection(period) {
    const till = period.closed_till;
    const ru = d => humanDate(d).split(',')[0];
    const input = el('input', {type: 'date', value: till || ''});
    const apply = async next => {
        if (till && (!next || next < till)) {
            const from = next ? ru(shiftDate(next, 1)) : 'начала учёта';
            if (!confirm(`Открыть период с ${from} по ${ru(till)}? Документы в нём снова можно будет менять, после правок может понадобиться перепроведение.`)) {
                return;
            }
        }
        if (await attempt(() => api('PUT', 'period', {closed_till: next}), next ? `Период закрыт по ${ru(next)}` : 'Все периоды открыты')) {
            route();
        }
    };
    return [
        el('h3', {}, 'Закрытие периода'),
        el('p', {class: 'muted'}, 'Документы по выбранную дату включительно нельзя создавать, изменять, удалять, проводить и отменять: '
            + 'случайная правка задним числом не потребует перепроведения. Закрыть период нельзя, пока документы в нём ждут перепроведения.'),
        el('p', {}, till ? el('strong', {}, `Закрыт по ${ru(till)}`) : 'Период не закрыт'),
        el('div', {class: 'toolbar'}, 'Закрыть по', input,
            el('button', {class: 'primary', onclick: () => input.value ? apply(input.value) : showMessage('Укажите дату')}, 'Закрыть период'),
            till ? el('button', {onclick: () => apply(null)}, 'Открыть все периоды') : null),
    ];
}

// ---------- печать ----------

const DOC_SIGNATURES = {
    order: [['Заявку принял', '']],
    production: [['Сдал (производство)', ''], ['Принял (склад)', 'storekeeper']],
    sale: [['Отпустил', 'storekeeper'], ['Получил', '']],
    return: [['Сдал (покупатель)', ''], ['Принял', 'storekeeper']],
    writeoff: [['Списал', 'storekeeper'], ['Утвердил (руководитель)', 'director']],
    inventory: [['Председатель комиссии', 'director'], ['Члены комиссии', ''], ['Материально ответственное лицо', 'storekeeper']],
    prices: [['Утвердил (руководитель)', 'director']],
};

/** Строка реквизитов: "ООО «Пекарня», ИНН 7707083893, КПП 773601001, г. Москва, тел. ...". */
function requisitesLine(name, inn, kpp, address, phone) {
    return [name, inn && 'ИНН ' + inn, kpp && 'КПП ' + kpp, address, phone && 'тел. ' + phone].filter(Boolean).join(', ');
}

/**
 * Печатная форма документа: шапка с реквизитами, таблица, итоги, сумма прописью, подписи.
 */
async function renderPrint(view, id, params) {
    const [doc, settings, items] = await Promise.all([api('GET', `docs/${id}`), api('GET', 'settings'), api('GET', 'items?kind=product&archived=1')]);
    const form = params.get('form');
    if (form === 'torg12' || form === 'upd') {
        if (doc.type !== 'sale') {
            throw new Error('ТОРГ-12 и УПД печатаются только для реализации');
        }
        const partner = doc.partner_id ? await api('GET', `partners/${doc.partner_id}`) : null;
        return (form === 'torg12' ? renderTorg12 : renderUpd)(view, doc, settings, partner, items);
    }
    const cfg = DOC_TYPES[doc.type];
    const partner = doc.partner_id ? await api('GET', `partners/${doc.partner_id}`) : null;
    const base = doc.base_id ? await api('GET', `docs/${doc.base_id}`) : null;
    const cols = cfg.columns;
    const withSum = cols.qty && cols.price;
    const withCost = cols.cost && !withSum;
    const vatPayer = settings.vat_payer === '1';
    const vatOf = new Map(items.map(i => [i.id, i.vat_rate]));
    const when = humanDate(doc.date).split(',')[0] + (cfg.time === false ? '' : ' ' + doc.time);

    let total = 0;
    let vatTotal = 0;
    const rows = doc.lines.map((l, i) => {
        const sum = withSum && l.price_kop !== null ? lineSum(l.qty, l.price_kop) : withCost && l.cost_kop !== null ? lineSum(l.qty, l.cost_kop) : null;
        const rate = vatOf.get(l.item_id);
        const vat = withSum && vatPayer && rate && sum !== null ? Math.round(sum * rate / (100 + rate)) : null;
        total += sum || 0;
        vatTotal += vat || 0;
        return el('tr', {},
            el('td', {class: 'num'}, i + 1),
            el('td', {}, l.name),
            el('td', {}, UNITS[l.unit]),
            cols.qty ? el('td', {class: 'num'}, formatQty(l.qty)) : null,
            withSum ? el('td', {class: 'num'}, l.price_kop === null ? '' : formatMoney(l.price_kop)) : null,
            withCost ? el('td', {class: 'num'}, l.cost_kop === null ? '' : formatMoney(l.cost_kop)) : null,
            !cols.qty && cols.price ? el('td', {class: 'num'}, formatMoney(l.price_kop)) : null,
            withSum && vatPayer ? el('td', {class: 'num'}, rate ? rate + '%' : 'без НДС') : null,
            withSum && vatPayer ? el('td', {class: 'num'}, vat === null ? '' : formatMoney(vat)) : null,
            sum !== null || withSum || withCost ? el('td', {class: 'num'}, sum === null ? '' : formatMoney(sum)) : null);
    });
    const money = withSum || withCost;
    const sumTitle = withCost ? 'Сумма по себестоимости' : 'Сумма';

    render(view,
        el('div', {class: 'toolbar no-print'},
            el('button', {class: 'primary', onclick: () => window.print()}, 'Печать'),
            el('button', {onclick: () => { location.hash = '#/docs/' + doc.id; }}, 'К документу')),
        el('div', {class: 'print-sheet'},
            el('p', {class: 'print-org'}, requisitesLine(settings.org_name || 'Организация не указана (раздел «Организация»)',
                settings.org_inn, settings.org_kpp, settings.org_address, settings.org_phone)),
            el('h2', {class: 'print-title'}, `${cfg.title} № ${doc.number} от ${when}`),
            partner ? el('p', {}, (doc.type === 'order' ? 'От кого: ' : doc.type === 'return' ? 'Покупатель: ' : 'Покупатель: ') +
                requisitesLine(partner.name, partner.inn, partner.kpp, partner.address, partner.phone)) : null,
            base ? el('p', {}, `Основание: реализация № ${base.number} от ${humanDate(base.date).split(',')[0]}`) : null,
            doc.comment ? el('p', {}, (cfg.comment === 'Причина' ? 'Причина: ' : 'Комментарий: ') + doc.comment) : null,
            el('table', {class: 'print-table'},
                el('thead', {}, el('tr', {},
                    el('th', {class: 'num'}, '№'),
                    el('th', {}, 'Наименование'),
                    el('th', {}, 'Ед.'),
                    cols.qty ? el('th', {class: 'num'}, 'Количество') : null,
                    withSum ? el('th', {class: 'num'}, 'Цена, ₽') : null,
                    withCost ? el('th', {class: 'num'}, 'Себестоимость, ₽') : null,
                    !cols.qty && cols.price ? el('th', {class: 'num'}, 'Цена, ₽') : null,
                    withSum && vatPayer ? el('th', {class: 'num'}, 'Ставка НДС') : null,
                    withSum && vatPayer ? el('th', {class: 'num'}, 'в т.ч. НДС, ₽') : null,
                    money ? el('th', {class: 'num'}, sumTitle + ', ₽') : null)),
                el('tbody', {}, rows)),
            money ? el('p', {class: 'print-total'},
                `Итого: ${formatMoney(total)} ₽` + (withSum ? (vatPayer ? `, в т.ч. НДС ${formatMoney(vatTotal)} ₽` : ', без НДС') : '')) : null,
            money ? el('p', {}, `Всего наименований ${doc.lines.length}, на сумму ${moneyInWords(total)}`) : null,
            el('div', {class: 'print-signs'}, (DOC_SIGNATURES[doc.type] || []).map(([role, key]) =>
                el('div', {}, `${role} ____________________ ${key && settings[key] ? '/' + settings[key] + '/' : '/____________/'}`))),
            settings.doc_footer ? el('p', {class: 'muted'}, settings.doc_footer) : null));
}

// ---------- ТОРГ-12 и УПД ----------

/** Коды единиц по ОКЕИ. */
const OKEI = {pcs: '796', kg: '166', l: '112'};

/** Число прописью мужского рода без единиц («номеров записей»): 2 -> "Два", 21 -> "Двадцать один". */
function countInWords(n) {
    return moneyInWords(n * 100).replace(/ рубл.*$/, '');
}

/**
 * Строки реализации с выделенным НДС: цены включают НДС, НДС = сумма × ставка / (100 + ставка),
 * цена без НДС = сумма без НДС / количество.
 */
function vatLines(doc, items, vatPayer) {
    const rateOf = new Map(items.map(i => [i.id, i.vat_rate]));
    return doc.lines.map((l, i) => {
        const total = lineSum(l.qty, l.price_kop || 0);
        const rate = vatPayer ? rateOf.get(l.item_id) : null;
        const vat = rate ? Math.round(total * rate / (100 + rate)) : 0;
        const net = total - vat;
        return {n: i + 1, line: l, total, rate, vat, net, price: Math.round(net * 1000 / l.qty)};
    });
}

const sumOf = (rows, key) => rows.reduce((acc, r) => acc + r[key], 0);
const money0 = kop => formatMoney(kop);
const rateText = r => (r === null || r === undefined ? 'без НДС' : r + '%');

/** Поле формы с подписью под линией: "должность / подпись / расшифровка". */
function signField(caption, value = '') {
    return el('span', {class: 'f-sign'}, el('span', {class: 'f-line'}, value), el('span', {class: 'f-caption'}, caption));
}

function formToolbar(doc) {
    return el('div', {class: 'toolbar no-print'},
        el('button', {class: 'primary', onclick: () => window.print()}, 'Печать'),
        el('button', {onclick: () => { location.hash = '#/docs/' + doc.id; }}, 'К документу'),
        el('span', {class: 'muted'}, 'Форма повторяет унифицированную; перед первым использованием покажите её бухгалтеру.'));
}

function orgFull(s) {
    return [s.org_name, s.org_inn && 'ИНН ' + s.org_inn, s.org_kpp && 'КПП ' + s.org_kpp, s.org_address, s.org_phone && 'тел. ' + s.org_phone,
        s.bank_account && `р/с ${s.bank_account}`, s.bank_name && 'в ' + s.bank_name, s.bank_bik && 'БИК ' + s.bank_bik,
        s.bank_corr_account && 'к/с ' + s.bank_corr_account].filter(Boolean).join(', ');
}

function partnerFull(p) {
    return p ? [p.name, p.inn && 'ИНН ' + p.inn, p.kpp && 'КПП ' + p.kpp, p.address, p.phone && 'тел. ' + p.phone].filter(Boolean).join(', ') : '';
}

/**
 * Товарная накладная по унифицированной форме ТОРГ-12 (постановление Госкомстата России от 25.12.98 № 132).
 */
function renderTorg12(view, doc, s, partner, items) {
    const vatPayer = s.vat_payer === '1';
    const rows = vatLines(doc, items, vatPayer);
    const date = humanDate(doc.date).split(',')[0];
    const total = sumOf(rows, 'total');
    const party = (label, value, caption) => el('tr', {},
        el('td', {class: 'f-label'}, label),
        el('td', {class: 'f-value'}, value, el('div', {class: 'f-caption'}, caption)));
    const head = ['Номер по по-рядку', 'Товар: наименование, характеристика, сорт, артикул товара', 'Товар: код', 'Единица измерения: наименование',
        'Единица измерения: код по ОКЕИ', 'Вид упаковки', 'Количество: в одном месте', 'Количество: мест, штук', 'Масса брутто',
        'Количество (масса нетто)', 'Цена, руб. коп.', 'Сумма без учета НДС, руб. коп.', 'НДС: ставка, %', 'НДС: сумма, руб. коп.',
        'Сумма с учетом НДС, руб. коп.'];
    const totalRow = label => el('tr', {class: 'f-total'},
        el('td', {colspan: 7, class: 'right'}, label), el('td'), el('td'),
        el('td', {class: 'num'}, formatQty(rows.reduce((a, r) => a + r.line.qty, 0))), el('td', {class: 'num'}, 'X'),
        el('td', {class: 'num'}, money0(sumOf(rows, 'net'))), el('td', {class: 'num'}, 'X'),
        el('td', {class: 'num'}, vatPayer ? money0(sumOf(rows, 'vat')) : '-'), el('td', {class: 'num'}, money0(total)));

    render(view, formToolbar(doc), el('div', {class: 'form-sheet landscape'},
        el('div', {class: 'f-form-code'}, 'Унифицированная форма № ТОРГ-12', el('br'), 'Утверждена постановлением Госкомстата России от 25.12.98 № 132'),
        el('div', {class: 'f-top'},
            el('table', {class: 'f-parties'}, el('tbody', {},
                party('', orgFull(s), 'организация-грузоотправитель, адрес, телефон, факс, банковские реквизиты'),
                party('', '', 'структурное подразделение'),
                party('Грузополучатель', partnerFull(partner), 'организация, адрес, телефон, факс, банковские реквизиты'),
                party('Поставщик', orgFull(s), 'организация, адрес, телефон, факс, банковские реквизиты'),
                party('Плательщик', partnerFull(partner), 'организация, адрес, телефон, факс, банковские реквизиты'),
                party('Основание', doc.comment, 'договор, заказ-наряд'))),
            el('table', {class: 'f-codes'}, el('tbody', {},
                el('tr', {}, el('td'), el('td', {class: 'f-code-head'}, 'Коды')),
                ...[['Форма по ОКУД', '0330212'], ['по ОКПО', s.org_okpo], ['Вид деятельности по ОКДП', ''], ['по ОКПО', ''],
                    ['по ОКПО', s.org_okpo], ['по ОКПО', ''], ['номер', ''], ['дата', ''], ['Транспортная накладная номер', ''],
                    ['дата', ''], ['Вид операции', '']].map(([k, v]) => el('tr', {}, el('td', {class: 'right'}, k), el('td', {class: 'f-code'}, v)))))),
        el('table', {class: 'f-docno'}, el('tbody', {},
            el('tr', {}, el('td', {rowspan: 2, class: 'f-doc-title'}, 'ТОВАРНАЯ НАКЛАДНАЯ'), el('th', {}, 'Номер документа'), el('th', {}, 'Дата составления')),
            el('tr', {}, el('td', {class: 'center'}, String(doc.number)), el('td', {class: 'center'}, date)))),
        el('table', {class: 'f-grid'},
            el('thead', {},
                el('tr', {}, head.map(h => el('th', {}, h))),
                el('tr', {}, head.map((_, i) => el('th', {}, String(i + 1))))),
            el('tbody', {},
                rows.map(r => el('tr', {},
                    el('td', {class: 'center'}, r.n), el('td', {}, r.line.name), el('td'),
                    el('td', {class: 'center'}, UNITS[r.line.unit]), el('td', {class: 'center'}, OKEI[r.line.unit]), el('td'), el('td'), el('td'), el('td'),
                    el('td', {class: 'num'}, formatQty(r.line.qty)), el('td', {class: 'num'}, money0(r.price)), el('td', {class: 'num'}, money0(r.net)),
                    el('td', {class: 'center'}, rateText(r.rate)), el('td', {class: 'num'}, r.rate ? money0(r.vat) : '-'), el('td', {class: 'num'}, money0(r.total)))),
                totalRow('Итого'),
                totalRow('Всего по накладной'))),
        el('p', {}, `Товарная накладная имеет приложение на ____ листах и содержит ${countInWords(rows.length).toLowerCase()} порядковых номеров записей`),
        el('p', {}, 'Всего отпущено на сумму ', el('strong', {}, moneyInWords(total))),
        el('div', {class: 'f-sign-cols'},
            el('div', {},
                el('p', {}, 'Отпуск груза разрешил ', signField('должность', 'Руководитель'), signField('подпись'), signField('расшифровка подписи', s.director)),
                el('p', {}, 'Главный (старший) бухгалтер ', signField('подпись'), signField('расшифровка подписи', s.accountant)),
                el('p', {}, 'Отпуск груза произвел ', signField('должность'), signField('подпись'), signField('расшифровка подписи', s.storekeeper)),
                el('p', {}, 'М.П.          «____» ______________ 20___ года')),
            el('div', {},
                el('p', {}, 'По доверенности № ________ от «____» ______________ 20___ года'),
                el('p', {}, 'выданной ', signField('кем, кому (организация, должность, фамилия, и. о.)')),
                el('p', {}, 'Груз принял ', signField('должность'), signField('подпись'), signField('расшифровка подписи')),
                el('p', {}, 'Груз получил грузополучатель ', signField('должность'), signField('подпись'), signField('расшифровка подписи')),
                el('p', {}, 'М.П.          «____» ______________ 20___ года')))));
}

/**
 * Универсальный передаточный документ: статус 1 (счёт-фактура и передаточный документ) для плательщика НДС,
 * иначе статус 2 (только передаточный документ).
 */
function renderUpd(view, doc, s, partner, items) {
    const vatPayer = s.vat_payer === '1';
    const rows = vatLines(doc, items, vatPayer);
    const date = humanDate(doc.date).split(',')[0];
    const field = (label, value, code) => el('tr', {}, el('td', {class: 'f-label'}, label), el('td', {class: 'f-value'}, value), el('td', {class: 'f-num'}, code));
    const innKpp = (inn, kpp) => [inn, kpp].filter(Boolean).join('/');
    const head = [['№ п/п', 'А'], ['Код товара/ работ, услуг', 'Б'], ['Наименование товара (описание выполненных работ, оказанных услуг), имущественного права', '1'],
        ['Код вида товара', '1а'], ['Единица измерения: код', '2'], ['Единица измерения: условное обозначение (национальное)', '2а'],
        ['Количество (объем)', '3'], ['Цена (тариф) за единицу измерения', '4'],
        ['Стоимость товаров (работ, услуг), имущественных прав без налога - всего', '5'], ['В том числе сумма акциза', '6'],
        ['Налоговая ставка', '7'], ['Сумма налога, предъявляемая покупателю', '8'],
        ['Стоимость товаров (работ, услуг), имущественных прав с налогом - всего', '9'],
        ['Страна происхождения товара: цифровой код', '10'], ['Страна происхождения товара: краткое наименование', '10а'],
        ['Регистрационный номер декларации на товары или регистрационный номер партии товара, подлежащего прослеживаемости', '11'],
        ['Количественная единица измерения товара, используемая в целях осуществления прослеживаемости: код', '12'],
        ['Количественная единица измерения ...: условное обозначение', '12а'], ['Количество товара, подлежащего прослеживаемости', '13'],
        ['Стоимость товара, подлежащего прослеживаемости, без НДС, в рублях', '14']];

    render(view, formToolbar(doc), el('div', {class: 'form-sheet landscape upd'},
        el('div', {class: 'f-top'},
            el('div', {class: 'upd-left'},
                el('div', {class: 'upd-title'}, 'Универсальный передаточный документ'),
                el('div', {}, 'Статус: ', el('span', {class: 'upd-status'}, vatPayer ? '1' : '2')),
                el('div', {class: 'f-caption'}, '1 - счет-фактура и передаточный документ (акт)', el('br'), '2 - передаточный документ (акт)')),
            el('table', {class: 'f-parties upd-head'}, el('tbody', {},
                field(el('strong', {}, 'Счет-фактура №'), `${doc.number} от ${date}`, '(1)'),
                field('Исправление №', '-- от --', '(1а)'),
                field(el('strong', {}, 'Продавец'), s.org_name, '(2)'),
                field('Адрес', s.org_address, '(2а)'),
                field('ИНН/КПП продавца', innKpp(s.org_inn, s.org_kpp), '(2б)'),
                field('Грузоотправитель и его адрес', 'он же', '(3)'),
                field('Грузополучатель и его адрес', partner ? [partner.name, partner.address].filter(Boolean).join(', ') : '', '(4)'),
                field('К платежно-расчетному документу №', '-- от --', '(5)'),
                field('Документ об отгрузке', `№ п/п 1-${rows.length} № ${doc.number} от ${date}`, '(5а)'),
                field(el('strong', {}, 'Покупатель'), partner ? partner.name : '', '(6)'),
                field('Адрес', partner ? partner.address : '', '(6а)'),
                field('ИНН/КПП покупателя', partner ? innKpp(partner.inn, partner.kpp) : '', '(6б)'),
                field('Валюта: наименование, код', 'Российский рубль, 643', '(7)'),
                field('Идентификатор государственного контракта, договора (соглашения) (при наличии)', '', '(8)')))),
        el('table', {class: 'f-grid'},
            el('thead', {},
                el('tr', {}, head.map(([h]) => el('th', {}, h))),
                el('tr', {}, head.map(([, n]) => el('th', {}, n)))),
            el('tbody', {},
                rows.map(r => el('tr', {},
                    el('td', {class: 'center'}, r.n), el('td', {class: 'center'}, '--'), el('td', {}, r.line.name), el('td', {class: 'center'}, '--'),
                    el('td', {class: 'center'}, OKEI[r.line.unit]), el('td', {class: 'center'}, UNITS[r.line.unit]),
                    el('td', {class: 'num'}, formatQty(r.line.qty)), el('td', {class: 'num'}, money0(r.price)), el('td', {class: 'num'}, money0(r.net)),
                    el('td', {class: 'center'}, 'без акциза'), el('td', {class: 'center'}, rateText(r.rate)),
                    el('td', {class: 'num'}, r.rate ? money0(r.vat) : 'без НДС'), el('td', {class: 'num'}, money0(r.total)),
                    el('td', {class: 'center'}, '--'), el('td', {class: 'center'}, '--'), el('td', {class: 'center'}, '--'),
                    el('td', {class: 'center'}, '--'), el('td', {class: 'center'}, '--'), el('td', {class: 'center'}, '--'), el('td', {class: 'center'}, '--'))),
                el('tr', {class: 'f-total'},
                    el('td', {colspan: 8}, el('strong', {}, 'Всего к оплате (9)')), el('td', {class: 'num'}, money0(sumOf(rows, 'net'))),
                    el('td', {colspan: 2, class: 'center'}, 'X'), el('td', {class: 'num'}, vatPayer ? money0(sumOf(rows, 'vat')) : 'без НДС'),
                    el('td', {class: 'num'}, money0(sumOf(rows, 'total'))), el('td', {colspan: 7})))),
        el('div', {class: 'f-sign-cols'},
            el('div', {},
                el('p', {}, 'Руководитель организации или иное уполномоченное лицо ', signField('подпись'), signField('ф.и.о.', s.director)),
                el('p', {}, 'Индивидуальный предприниматель или иное уполномоченное лицо ', signField('подпись'), signField('ф.и.о.'))),
            el('div', {},
                el('p', {}, 'Главный бухгалтер или иное уполномоченное лицо ', signField('подпись'), signField('ф.и.о.', s.accountant)),
                el('p', {}, signField('реквизиты свидетельства о государственной регистрации индивидуального предпринимателя')))),
        el('table', {class: 'f-parties upd-bottom'}, el('tbody', {},
            el('tr', {}, el('td', {colspan: 2}, 'Основание передачи (сдачи) / получения (приемки) ', signField('договор; доверенность и др.', doc.comment), ' [8]')),
            el('tr', {}, el('td', {colspan: 2}, 'Данные о транспортировке и грузе ', signField('транспортная накладная, поручение экспедитору, экспедиторская / складская расписка и др. / масса нетто/ брутто груза, если не приведены ссылки на транспортные документы, содержащие эти сведения'), ' [9]')),
            el('tr', {},
                el('td', {},
                    el('p', {}, 'Товар (груз) передал / услуги, результаты работ, права сдал [10]'),
                    el('p', {}, signField('должность'), signField('подпись'), signField('ф.и.о.', s.storekeeper)),
                    el('p', {}, 'Дата отгрузки, передачи (сдачи) [11] ', el('span', {class: 'f-line'}, date)),
                    el('p', {}, 'Иные сведения об отгрузке, передаче [12] ', signField('ссылки на неотъемлемые приложения, сопутствующие документы, иные документы и т.п.')),
                    el('p', {}, 'Ответственный за правильность оформления факта хозяйственной жизни [13]'),
                    el('p', {}, signField('должность'), signField('подпись'), signField('ф.и.о.', s.accountant)),
                    el('p', {}, 'Наименование экономического субъекта – составителя документа (в т.ч. комиссионера / агента) [14]'),
                    el('p', {}, el('span', {class: 'f-line'}, [s.org_name, innKpp(s.org_inn, s.org_kpp) && 'ИНН/КПП ' + innKpp(s.org_inn, s.org_kpp)].filter(Boolean).join(', '))),
                    el('p', {}, 'М.П.')),
                el('td', {},
                    el('p', {}, 'Товар (груз) получил / услуги, результаты работ, права принял [15]'),
                    el('p', {}, signField('должность'), signField('подпись'), signField('ф.и.о.')),
                    el('p', {}, 'Дата получения (приемки) [16] ', el('span', {class: 'f-line'}, '«____» ______________ 20___ г.')),
                    el('p', {}, 'Иные сведения о получении, приемке [17] ', signField('информация о наличии/отсутствии претензии; ссылки на неотъемлемые приложения, и другие документы и т.п.')),
                    el('p', {}, 'Ответственный за правильность оформления факта хозяйственной жизни [18]'),
                    el('p', {}, signField('должность'), signField('подпись'), signField('ф.и.о.')),
                    el('p', {}, 'Наименование экономического субъекта – составителя документа [19]'),
                    el('p', {}, el('span', {class: 'f-line'}, partner ? [partner.name, innKpp(partner.inn, partner.kpp) && 'ИНН/КПП ' + innKpp(partner.inn, partner.kpp)].filter(Boolean).join(', ') : '')),
                    el('p', {}, 'М.П.')))))));
}

// ---------- товарный отчёт ----------

/**
 * Товарный отчёт за период по образцу ТОРГ-29: остаток, приход и расход по документам в суммах
 * по себестоимости, остаток на конец; приложение - количество по позициям.
 */
async function renderReport(view, params) {
    const today = isoDate(new Date());
    const from = params.get('from') || today.slice(0, 8) + '01';
    const to = params.get('to') || today;
    const groupId = params.get('group') ? Number(params.get('group')) : null;
    const priceTypeId = params.get('price_type') ? Number(params.get('price_type')) : null;
    const [report, settings, groups, priceTypes] = await Promise.all([
        api('GET', `report/goods?from=${from}&to=${to}` + (groupId ? '&group=' + groupId : '') + (priceTypeId ? '&price_type=' + priceTypeId : '')),
        api('GET', 'settings'),
        api('GET', 'groups'),
        api('GET', 'price-types'),
    ]);
    const priceType = priceTypes.find(t => t.id === priceTypeId);
    const go = changes => {
        const p = {from, to, group: groupId, price_type: priceTypeId, ...changes};
        location.hash = '#/report?' + Object.entries(p).filter(([, v]) => v).map(([k, v]) => `${k}=${v}`).join('&');
    };
    const d = iso => humanDate(iso).split(',')[0];
    const money = (cost, price) => [el('td', {class: 'num'}, formatMoney(cost)), priceType ? el('td', {class: 'num'}, formatMoney(price)) : null];
    const docRow = r => el('tr', {},
        el('td', {}, r.type === 'prices' ? 'Переоценка (установка цен)'
            : (r.revaluation ? (r.incoming ? 'Переоценка: продано дороже цены' : 'Переоценка: продано дешевле цены') : DOC_TYPES[r.type].title) +
              (r.partner_name ? ', ' + r.partner_name : '')),
        el('td', {class: 'center'}, d(r.date) + ' ' + r.time),
        el('td', {class: 'center'}, el('a', {href: '#/docs/' + r.id}, String(r.number))),
        money(r.sum, r.price_sum),
        el('td'));
    const sumRow = (text, cost, price) => el('tr', {class: 'f-total'}, el('td', {colspan: 3}, text), money(cost, price), el('td'));
    const section = text => el('tr', {}, el('td', {colspan: priceType ? 6 : 5}, el('strong', {}, text)));
    // переоценка при продаже - строка к реализации, а не отдельный документ
    const docs = [...report.incoming, ...report.outgoing].filter(r => !r.revaluation).length;

    render(view,
        el('div', {class: 'toolbar no-print'},
            'с', el('input', {type: 'date', value: from, onchange: e => e.target.value && go({from: e.target.value})}),
            'по', el('input', {type: 'date', value: to, onchange: e => e.target.value && go({to: e.target.value})}),
            'группа', el('select', {onchange: e => go({group: selectValue(e.target)})}, groupOptions(groups, groupId, 'все')),
            'и по ценам', el('select', {onchange: e => go({price_type: selectValue(e.target)})},
                el('option', {value: '', selected: !priceTypeId}, '— только себестоимость —'),
                priceTypes.filter(t => !t.system).map(t => el('option', {value: t.id, selected: t.id === priceTypeId}, t.name))),
            el('button', {class: 'primary', onclick: () => window.print()}, 'Печать')),
        report.lines_without_cost ? el('div', {class: 'warnings no-print'},
            `⚠ движений без себестоимости: ${report.lines_without_cost}, они оценены нулём. Установите себестоимость и перепроведите документы.`) : null,
        priceType && report.moves_without_price ? el('div', {class: 'warnings no-print'},
            `⚠ движений без цены «${priceType.name}»: ${report.moves_without_price}, по этому типу они оценены нулём.`) : null,
        el('div', {class: 'print-sheet'},
            el('p', {class: 'print-org'}, requisitesLine(settings.org_name || 'Организация не указана', settings.org_inn, settings.org_kpp, settings.org_address)),
            el('h2', {class: 'print-title'}, `Товарный отчёт за период с ${d(from)} по ${d(to)}`),
            groupId ? el('p', {}, 'Группа: ' + groupPath(groups, groupId)) : null,
            el('p', {}, 'Материально ответственное лицо: ' + (settings.storekeeper || '____________________')),
            el('table', {class: 'print-table'},
                el('thead', {}, el('tr', {},
                    el('th', {}, 'Наименование'), el('th', {}, 'Дата'), el('th', {}, 'Номер'),
                    el('th', {class: 'num'}, 'Сумма по себестоимости, ₽'),
                    priceType ? el('th', {class: 'num'}, `Сумма по ценам «${priceType.name}», ₽`) : null,
                    el('th', {}, 'Отметки бухгалтерии'))),
                el('tbody', {},
                    sumRow(`Остаток на ${d(from)}`, report.opening_sum, report.opening_price_sum),
                    section('Приход'),
                    report.incoming.map(docRow),
                    sumRow('Итого по приходу', report.in_sum, report.in_price_sum),
                    sumRow('Итого с остатком', report.opening_sum + report.in_sum, report.opening_price_sum + report.in_price_sum),
                    section('Расход'),
                    report.outgoing.map(docRow),
                    sumRow('Итого по расходу', report.out_sum, report.out_price_sum),
                    sumRow(`Остаток на ${d(to)}`, report.closing_sum, report.closing_price_sum))),
            el('p', {}, `Приложение: ${docs} ${plural(docs, ['документ', 'документа', 'документов'])}`),
            el('div', {class: 'print-signs'},
                el('div', {}, 'Отчёт с документами принял и проверил ____________________ /' + (settings.accountant || '____________') + '/'),
                el('div', {}, 'Материально ответственное лицо ____________________ /' + (settings.storekeeper || '____________') + '/'))));
}

// ---------- остатки ----------

async function renderStock(view, params) {
    const date = params.get('date') || isoDate(new Date());
    const groupId = params.get('group') ? Number(params.get('group')) : null;
    const groups = await api('GET', 'groups');
    const balances = await api('GET', 'stock?date=' + date + (groupId ? '&group=' + groupId : ''));
    const go = (d, g = groupId) => { location.hash = '#/stock?date=' + d + (g ? '&group=' + g : ''); };
    const groupSelect = el('select', {onchange: e => go(date, selectValue(e.target))}, groupOptions(groups, groupId, 'все группы'));
    render(view,
        el('h2', {}, 'Остатки'),
        el('div', {class: 'toolbar'},
            el('button', {title: 'Предыдущий день', onclick: () => go(shiftDate(date, -1))}, '←'),
            el('input', {type: 'date', value: date, onchange: e => e.target.value && go(e.target.value)}),
            el('button', {title: 'Следующий день', onclick: () => go(shiftDate(date, 1))}, '→'),
            el('span', {class: 'muted'}, 'на конец дня ' + humanDate(date))),
        el('div', {class: 'toolbar'}, 'Группа', groupSelect,
            el('button', {onclick: () => { location.hash = '#/report' + (groupId ? '?group=' + groupId : ''); }}, 'Товарный отчёт за период')),
        el('div', {class: 'table-wrap'}, el('table', {class: 'needs'},
            el('thead', {}, el('tr', {}, el('th', {}, 'Продукция'), el('th', {}, 'Группа'), el('th', {class: 'num'}, 'Остаток'), el('th', {}, 'Ед.'))),
            el('tbody', {}, balances.length
                ? balances.map(b => el('tr', {class: b.archived ? 'archived' : null},
                    el('td', {}, b.name, b.archived ? el('span', {class: 'muted'}, ' (архив)') : null),
                    el('td', {class: 'muted'}, b.group_id ? groupPath(groups, b.group_id) : ''),
                    el('td', {class: 'num'}, formatQty(b.qty)),
                    el('td', {}, UNITS[b.unit])))
                : el('tr', {}, el('td', {colspan: 4, class: 'muted'}, 'Остатков нет'))))));
}

// ---------- перепроведение ----------

/** Момент "2026-10-02 08:00:00" для людей: "02.10.2026 08:00", без времени, если оно 00:00. */
function humanMoment(moment) {
    const time = moment.slice(11, 16);
    return moment.slice(0, 10).split('-').reverse().join('.') + (time && time !== '00:00' ? ' ' + time : '');
}

/** Предупреждение над страницей, если документы изменены задним числом и нужно перепроведение. */
/** Ссылка на документ, изменённый задним числом; установку цен продавцу только названием. */
function causeLink(cause) {
    return cause.type === 'prices' && !canManage() ? cause.title : el('a', {href: '#/docs/' + cause.id}, cause.title);
}

async function refreshRepostBanner() {
    const box = document.getElementById('repost-banner');
    try {
        const state = await api('GET', 'repost');
        const onPage = location.hash.startsWith('#/repost');
        box.hidden = !state.needed || onPage;
        if (state.needed) {
            render(box,
                '⚠ ',
                state.cause ? ['Документ ', causeLink(state.cause), ' изменён задним числом. '] : 'Документы изменены задним числом. ',
                `Нужно перепровести документы с ${humanMoment(state.from.moment)} (документов: ${state.count}). `,
                el('a', {href: '#/repost'}, 'Перепровести'));
        }
    } catch {
        box.hidden = true;
    }
}

/**
 * Перепроведение всех документов с момента: по одному запросу на документ, с прогрессом;
 * на ошибке остановка с документом и причиной, «Продолжить» - с того же документа.
 */
async function renderRepost(view, params) {
    const state = await api('GET', 'repost');
    if (!canManage()) {
        render(view,
            el('h2', {}, 'Перепроведение документов'),
            el('p', {}, state.needed
                ? `Нужно перепровести документы с ${humanMoment(state.from.moment)} (документов: ${state.count}). Перепроводит технолог или администратор: сообщите им.`
                : 'Перепроводить нечего.'));
        return;
    }
    // незавершённый прогон хранится на сервере: продолжить его можно после перезагрузки и с другого компьютера
    const saved = state.run;
    const start = saved ? saved.from : state.needed ? state.from.moment : isoDate(new Date()) + ' 00:00:00';
    const dateInput = el('input', {type: 'date', value: params.get('date') || start.slice(0, 10)});
    const timeInput = el('input', {type: 'time', value: params.get('time') || start.slice(11, 16)});
    const countText = el('span', {class: 'muted'});
    const status = el('div', {class: 'repost-progress'});
    let running = false;
    let stopRequested = false;
    // момент полей ввода совпадает с прогоном на сервере: шаг продолжит его, иначе начнёт новый
    const moment = () => `${dateInput.value} ${timeInput.value || '00:00'}:00`;
    const continues = () => Boolean(saved) && moment() === saved.from;

    const updateCount = async () => {
        if (!dateInput.value) {
            return;
        }
        const {count} = await api('GET', `repost/count?date=${dateInput.value}&time=${timeInput.value || '00:00'}`);
        countText.textContent = `документов к перепроведению: ${count}`;
    };
    const startButton = el('button', {class: 'primary', onclick: () => run(false)});
    const restartButton = el('button', {onclick: () => run(true)}, 'Начать заново');
    const stopButton = el('button', {hidden: true, onclick: () => { stopRequested = true; }}, 'Остановить');
    const updateButtons = () => {
        startButton.textContent = continues() ? 'Продолжить' : 'Перепровести';
        restartButton.hidden = !continues();
    };
    const changed = () => { updateButtons(); updateCount().catch(e => showMessage(e.message)); };
    dateInput.addEventListener('change', changed);
    timeInput.addEventListener('change', changed);

    const errorBlock = (doc, text) => [
        el('div', {class: 'repost-error'}, 'Остановлено на документе ', el('a', {href: '#/docs/' + doc.id}, doc.title), ': ', text),
        el('p', {class: 'muted'}, 'Исправьте документ или те, что ему мешают, и нажмите «Продолжить»: перепроведение начнётся с него.'),
    ];
    if (saved) {
        render(status,
            el('div', {class: 'warnings'}, `Перепроведение с ${humanMoment(saved.from)} не закончено: перепроведено ${saved.done}, осталось ${saved.remaining}` +
                (saved.after && saved.after.title ? `, последним - ${saved.after.title}` : '') + `. Последний шаг ${humanMoment(saved.updated)}.`),
            saved.error ? errorBlock(saved.error.doc, saved.error.text) : null);
    }

    const run = async restart => {
        if (running || !dateInput.value) {
            return;
        }
        running = true;
        stopRequested = false;
        startButton.disabled = restartButton.disabled = dateInput.disabled = timeInput.disabled = true;
        stopButton.hidden = false;
        showMessage('');
        const body = {date: dateInput.value, time: timeInput.value || '00:00', restart};
        const bar = el('progress', {max: 1, value: 0});
        const counter = el('div', {});
        const current = el('div', {class: 'muted'});
        render(status, bar, counter, current);
        let done = 0;
        let finish = null;
        try {
            for (;;) {
                if (stopRequested) {
                    finish = () => render(status, el('p', {}, `Остановлено: перепроведено ${done}. «Продолжить» - со следующего документа, в том числе после перезагрузки страницы.`));
                    break;
                }
                const step = await api('POST', 'repost/step', body);
                body.restart = false;
                done = step.done;
                if (!step.doc) {
                    finish = () => render(status,
                        el('p', {class: 'ok-text'}, done ? `Готово: перепроведено документов ${done}.` : 'Документов с этого момента нет.'),
                        step.needed ? el('div', {class: 'warnings'}, '⚠ Пока шло перепроведение, документы снова изменили задним числом: перепроведите ещё раз.') : null);
                    break;
                }
                const total = done + (step.error ? 1 : 0) + step.remaining;
                bar.max = total;
                bar.value = done;
                if (step.error) {
                    finish = () => render(status, bar, counter, errorBlock(step.doc, step.error));
                    break;
                }
                counter.textContent = `Перепроведено ${done} из ${total}`;
                current.textContent = step.doc.title;
            }
        } catch (e) {
            showMessage(e.message);
        }
        running = false;
        // состояние прогона после остановки или конца - с сервера
        const fresh = (await api('GET', 'repost').catch(() => ({run: null}))).run;
        state.run = fresh;
        startButton.disabled = restartButton.disabled = dateInput.disabled = timeInput.disabled = false;
        startButton.textContent = fresh && fresh.from === moment() ? 'Продолжить' : 'Перепровести';
        restartButton.hidden = !(fresh && fresh.from === moment());
        stopButton.hidden = true;
        if (finish) {
            finish();
        }
        refreshRepostBanner();
    };

    updateButtons();
    render(view,
        el('a', {class: 'back', href: '#/docs'}, '← Документы'),
        el('h2', {}, 'Перепроведение документов'),
        state.needed
            ? el('div', {class: 'warnings'}, '⚠ ',
                state.cause ? ['Документ ', el('a', {href: '#/docs/' + state.cause.id}, state.cause.title), ' изменён задним числом. '] : '',
                `Документы с ${humanMoment(state.from.moment)} нужно перепровести.`)
            : el('p', {class: 'muted'}, 'Изменений задним числом нет. Перепровести можно с любого момента.'),
        el('p', {class: 'muted'}, 'Проведённые документы перепроводятся по порядку времени: себестоимость строк - на момент документа, ' +
            'инвентаризация - заново от учётного остатка. На первой ошибке перепроведение остановится. Место остановки хранится ' +
            'на сервере: продолжить можно после перезагрузки страницы и с другого компьютера.'),
        el('div', {class: 'toolbar'}, 'с', dateInput, timeInput, startButton, restartButton, stopButton, countText),
        status);
    await updateCount();
}

// ---------- расчёт потребности ----------

function needsTable(title, needs, note) {
    if (!needs.length) {
        return null;
    }
    return el('section', {},
        el('h3', {}, title),
        note ? el('p', {class: 'muted'}, note) : null,
        el('div', {class: 'table-wrap'}, el('table', {class: 'needs'},
            el('thead', {}, el('tr', {}, el('th', {}, 'Наименование'), el('th', {class: 'num'}, 'Количество'), el('th', {}, 'Ед.'))),
            el('tbody', {}, needs.map(n => el('tr', {},
                el('td', {}, n.name),
                el('td', {class: 'num'}, formatQty(n.qty)),
                el('td', {}, UNITS[n.unit])))))));
}

async function renderRequirements(view, params) {
    const docs = params.get('docs') || '';
    const date = params.get('date');
    const [req, orders] = await Promise.all([
        api('GET', 'requirements?docs=' + encodeURIComponent(docs)),
        Promise.all(docs.split(',').filter(Boolean).map(id => api('GET', 'docs/' + id))),
    ]);
    render(view,
        el('a', {class: 'back no-print', href: '#/orders' + (date ? '?date=' + date : '')}, '← Заявки'),
        el('div', {class: 'toolbar section-head', style: 'margin-top: 0'},
            el('h2', {}, 'Потребность' + (date ? ' на ' + humanDate(date) : '')),
            el('button', {class: 'primary no-print', onclick: () => window.print()}, 'Печать')),
        el('p', {class: 'muted'}, 'Заявки: ' + orders.map(o => `№ ${o.number}${orderFrom(o) ? ' (' + orderFrom(o) + ')' : ''}`).join(', ')),
        req.warnings.length ? el('div', {class: 'warnings'}, req.warnings.map(w => el('div', {}, '⚠ ' + w))) : null,
        needsTable('Произвести', req.products),
        needsTable('Полуфабрикаты', req.intermediates, 'Сколько приготовить, чтобы произвести продукцию выше.'),
        needsTable('Сырьё', req.raw, req.raw.some(n => n.unit === 'pcs') ? 'Штучное сырьё округлено вверх до целых.' : null));
}

// ---------- маршрутизация ----------

// ---------- вход и пользователи ----------

/** Узнаёт, кто вошёл: показывает интерфейс по роли или экран входа. */
async function start() {
    let auth;
    try {
        auth = await api('GET', 'auth');
    } catch (e) {
        showMessage('сервер недоступен: ' + e.message);
        return;
    }
    if (!auth.user) {
        renderLogin(auth.setup);
        return;
    }
    me = auth.user;
    document.body.classList.remove('logged-out');
    const level = {seller: 0, technologist: 1, admin: 2};
    for (const a of document.querySelectorAll('nav a[data-role]')) {
        a.hidden = level[me.role] < level[a.dataset.role];
    }
    render(document.getElementById('user-menu'),
        el('a', {href: '#/password', title: 'Сменить пароль'}, me.name || me.login),
        el('span', {class: 'muted'}, ' ' + ROLES[me.role].toLowerCase() + ' '),
        el('button', {onclick: logout}, 'Выйти'));
    route();
    refreshRepostBanner();
}

async function logout() {
    await api('POST', 'auth/logout').catch(() => {});
    me = null;
    renderLogin(false);
}

/**
 * Экран входа; в пустой базе - создание первого пользователя (администратора).
 */
function renderLogin(setup) {
    document.body.classList.add('logged-out');
    document.getElementById('repost-banner').hidden = true;
    const login = el('input', {name: 'login', autocomplete: 'username', required: true, size: 24});
    const name = el('input', {name: 'name', size: 24, placeholder: 'Как показывать в документах'});
    const password = el('input', {name: 'password', type: 'password', autocomplete: setup ? 'new-password' : 'current-password', required: true, size: 24});
    const repeat = el('input', {type: 'password', autocomplete: 'new-password', required: true, size: 24});
    const form = el('form', {
        class: 'login',
        onsubmit: async e => {
            e.preventDefault();
            if (setup && password.value !== repeat.value) {
                showMessage('пароли не совпадают');
                return;
            }
            if (await attempt(() => setup
                ? api('POST', 'auth/setup', {login: login.value, name: name.value, password: password.value})
                : api('POST', 'auth/login', {login: login.value, password: password.value}))) {
                showMessage('');
                start();
            }
        },
    },
    el('h2', {}, setup ? 'Первый запуск' : 'Вход'),
    setup ? el('p', {class: 'muted'}, 'Пользователей ещё нет. Создайте администратора: он заведёт технологов и продавцов в разделе «Пользователи».') : null,
    el('label', {}, 'Логин', login),
    setup ? el('label', {}, 'Имя', name) : null,
    el('label', {}, 'Пароль', password),
    setup ? el('label', {}, 'Пароль ещё раз', repeat) : null,
    el('button', {class: 'primary'}, setup ? 'Создать администратора' : 'Войти'));
    render(document.getElementById('view'), form);
    login.focus();
}

/** Смена своего пароля. */
async function renderPassword(view) {
    const old = el('input', {type: 'password', autocomplete: 'current-password', size: 24});
    const fresh = el('input', {type: 'password', autocomplete: 'new-password', size: 24});
    const repeat = el('input', {type: 'password', autocomplete: 'new-password', size: 24});
    const save = async () => {
        if (fresh.value !== repeat.value) {
            showMessage('новые пароли не совпадают');
            return;
        }
        if (await attempt(() => api('PUT', 'auth/password', {old_password: old.value, password: fresh.value}),
            'Пароль изменён, на других устройствах нужно войти заново')) {
            old.value = fresh.value = repeat.value = '';
        }
    };
    render(view,
        el('h2', {}, 'Пароль'),
        el('p', {class: 'muted'}, `${me.name || me.login}, ${ROLES[me.role].toLowerCase()}, логин ${me.login}`),
        el('div', {class: 'fields'},
            el('label', {}, 'Текущий пароль', old),
            el('label', {}, 'Новый пароль', fresh),
            el('label', {}, 'Новый ещё раз', repeat),
            el('button', {class: 'primary', onclick: save}, 'Сменить пароль')));
}

/**
 * Пользователи (администратор): добавление, имя, роль, блокировка, новый пароль.
 */
async function renderUsers(view) {
    const users = await api('GET', 'users');
    const login = el('input', {placeholder: 'Логин', required: true, size: 16});
    const name = el('input', {placeholder: 'Имя', size: 20});
    const role = el('select', {}, options(ROLES, 'seller'));
    const password = el('input', {type: 'password', placeholder: 'Пароль', autocomplete: 'new-password', required: true, size: 14});
    const form = el('form', {
        class: 'inline',
        onsubmit: async e => {
            e.preventDefault();
            if (await attempt(() => api('POST', 'users', {login: login.value, name: name.value, role: role.value, password: password.value}),
                'Пользователь добавлен')) {
                route();
            }
        },
    }, login, name, role, password, el('button', {class: 'primary'}, 'Добавить'));

    const rows = users.map(u => {
        const uname = el('input', {value: u.name, size: 20});
        const urole = el('select', {}, options(ROLES, u.role));
        const blocked = el('input', {type: 'checkbox', checked: u.blocked});
        const save = async () => {
            if (await attempt(() => api('PUT', `users/${u.id}`, {name: uname.value, role: urole.value, blocked: blocked.checked}), 'Сохранено')) {
                route();
            }
        };
        const setPassword = async () => {
            const value = prompt(`Новый пароль для «${u.login}»:`);
            if (value) {
                await attempt(() => api('PUT', `users/${u.id}`, {password: value}), `Пароль «${u.login}» изменён`);
            }
        };
        return el('tr', {class: u.blocked ? 'archived' : null},
            el('td', {}, u.login, u.id === me.id ? el('span', {class: 'muted'}, ' (вы)') : null),
            el('td', {}, uname),
            el('td', {}, urole),
            el('td', {}, el('label', {class: 'check'}, blocked, 'заблокирован')),
            el('td', {}, el('div', {class: 'toolbar', style: 'margin: 0'},
                el('button', {class: 'primary', onclick: save}, 'Сохранить'),
                el('button', {onclick: setPassword}, 'Пароль'))));
    });

    render(view,
        el('h2', {}, 'Пользователи'),
        el('p', {class: 'muted'}, 'Продавец: заявки, поступление, реализация, возврат, списание, инвентаризация, клиенты; номенклатуру и остатки только смотрит, цен и установок цен не видит. ' +
            'Технолог: всё, кроме пользователей. Администратор: всё. Заблокированный не может войти.'),
        form,
        el('div', {class: 'table-wrap', style: 'margin-top: 12px'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Логин'), el('th', {}, 'Имя'), el('th', {}, 'Роль'), el('th', {}, ''), el('th', {}, ''))),
            el('tbody', {}, rows))));
}

/** Размер файла: "512 КБ", "3,4 МБ". */
function formatSize(bytes) {
    return bytes < 1024 * 1024
        ? Math.max(1, Math.round(bytes / 1024)) + ' КБ'
        : (bytes / 1024 / 1024).toFixed(1).replace('.', ',') + ' МБ';
}

/**
 * Резервные копии (администратор): копия сейчас, расписание, скачивание и удаление.
 */
async function renderBackups(view) {
    const state = await api('GET', 'backups');
    const s = state.schedule;
    const enabled = el('input', {type: 'checkbox', checked: s.enabled});
    const time = el('input', {type: 'time', value: s.time});
    const keep = el('input', {type: 'number', min: 1, max: 365, value: s.keep, style: 'width: 5em'});
    const saveSchedule = async () => {
        if (await attempt(() => api('PUT', 'backups/schedule', {enabled: enabled.checked, time: time.value, keep: Number(keep.value)}),
            'Расписание сохранено')) {
            route();
        }
    };
    const createNow = async () => {
        if (await attempt(() => api('POST', 'backups'), 'Копия сделана')) {
            route();
        }
    };
    const remove = async name => {
        if (confirm(`Удалить копию ${name}?`) && await attempt(() => api('DELETE', `backups/${name}`), 'Копия удалена')) {
            route();
        }
    };

    render(view,
        el('div', {class: 'toolbar section-head', style: 'margin-top: 0'},
            el('h2', {}, 'Резервные копии'),
            el('button', {class: 'primary', onclick: createNow}, 'Сделать копию сейчас')),
        el('p', {class: 'muted'}, `Копии хранятся на сервере в ${state.dir}. Скачайте копию, чтобы держать её ещё и на другом компьютере.`),
        el('h3', {}, 'Расписание'),
        el('div', {class: 'toolbar'},
            el('label', {class: 'check'}, enabled, 'каждый день в'), time,
            el('span', {}, 'хранить последних'), keep,
            el('button', {onclick: saveSchedule}, 'Сохранить')),
        el('p', {class: 'muted'}, 'Хранение касается только копий по расписанию: ручные остаются, пока их не удалить.'),
        s.last_error ? el('p', {class: 'warnings'}, `Копия по расписанию ${humanMoment(s.last_run)} не удалась: ${s.last_error}`) : null,
        el('div', {class: 'table-wrap', style: 'margin-top: 12px'}, el('table', {},
            el('thead', {}, el('tr', {}, el('th', {}, 'Когда'), el('th', {}, 'Вид'), el('th', {class: 'num'}, 'Размер'), el('th', {}))),
            el('tbody', {}, state.backups.length
                ? state.backups.map(b => el('tr', {},
                    el('td', {}, humanMoment(b.created)),
                    el('td', {}, b.scheduled ? 'по расписанию' : 'вручную'),
                    el('td', {class: 'num'}, formatSize(b.size)),
                    el('td', {}, el('div', {class: 'toolbar', style: 'margin: 0'},
                        el('a', {class: 'button', href: `/api/backups/${b.name}`, download: b.name}, 'Скачать'),
                        el('button', {class: 'danger', onclick: () => remove(b.name)}, 'Удалить')))))
                : el('tr', {}, el('td', {colspan: 4, class: 'muted'}, 'Копий пока нет'))))));
}

async function route() {
    const view = document.getElementById('view');
    const [hash, query] = (location.hash || '#/items').split('?');
    const params = new URLSearchParams(query || '');
    const section = {requirements: 'orders', report: 'stock', repost: 'docs'}[hash.split('/')[1]] || hash.split('/')[1];
    document.body.classList.toggle('print-page', section === 'print');
    for (const a of document.querySelectorAll('nav a[data-section]')) {
        a.classList.toggle('active', a.dataset.section === section);
    }
    try {
        let m;
        if ((m = hash.match(/^#\/items\/(\d+)$/))) {
            await renderItem(view, Number(m[1]));
        } else if (hash === '#/orders') {
            await renderOrders(view, params);
        } else if (hash === '#/orders/new') {
            params.set('type', 'order');
            await renderDoc(view, null, params);
        } else if ((m = hash.match(/^#\/(?:orders|docs)\/(\d+)$/))) {
            await renderDoc(view, Number(m[1]), params);
        } else if (hash === '#/docs/new') {
            await renderDoc(view, null, params);
        } else if (hash === '#/docs') {
            await renderDocs(view, params);
        } else if ((m = hash.match(/^#\/print\/(\d+)$/))) {
            await renderPrint(view, Number(m[1]), params);
        } else if (hash === '#/settings') {
            await renderSettings(view);
        } else if (hash === '#/partners') {
            await renderPartners(view);
        } else if ((m = hash.match(/^#\/partners\/(\d+)$/))) {
            await renderPartner(view, Number(m[1]));
        } else if (hash === '#/prices') {
            await renderPrices(view, params);
        } else if (hash === '#/report') {
            await renderReport(view, params);
        } else if (hash === '#/stock') {
            await renderStock(view, params);
        } else if (hash === '#/repost') {
            await renderRepost(view, params);
        } else if (hash === '#/requirements') {
            await renderRequirements(view, params);
        } else if (hash === '#/users') {
            await renderUsers(view);
        } else if (hash === '#/backups') {
            await renderBackups(view);
        } else if (hash === '#/password') {
            await renderPassword(view);
        } else {
            await renderItems(view);
        }
    } catch (e) {
        showMessage(e.message);
    }
}

window.addEventListener('hashchange', () => {
    showMessage('');
    if (me) {
        route();
        refreshRepostBanner();
    }
});
start();

api('GET', 'version')
    .then(v => document.getElementById('version').textContent = `bakery ${v.version}, схема ${v.schema}`)
    .catch(e => document.getElementById('version').textContent = 'сервер недоступен: ' + e.message);
