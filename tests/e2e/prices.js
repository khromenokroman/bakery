// Цены: технолог создаёт тип «Вариант 1», ставит цену батона 45,50 и себестоимость 31,20; в новом
// поступлении себестоимость и сумма появляются сразу при выборе батона, до сохранения.
window.prompt = () => 'Вариант 1';
const pick = (row, text) => {
    const select = row.querySelector('select');
    select.value = [...select.options].find(o => o.text === text).value;
    select.dispatchEvent(new Event('change'));
};
const setPrice = async (typeName, price) => {
    [...document.querySelectorAll('.tabs button')].find(b => b.textContent === typeName).click();
    await sleep(500);
    btn('Новая установка цен').click();
    await sleep(600);
    const typeSelect = [...document.querySelectorAll('.fields label')].find(l => l.textContent.startsWith('Тип цен')).querySelector('select');
    expect(typeSelect.selectedOptions[0].text === typeName, 'в установке цен не выбран тип ' + typeName + ': ' + typeSelect.selectedOptions[0].text);
    const row = document.querySelector('tbody tr');
    pick(row, 'Батон');
    row.querySelector('input.money').value = price;
    btn('Сохранить и провести').click();
    await sleep(800);
    expect(document.querySelector('.status.posted'), 'установка цен не проведена: ' + document.getElementById('message').textContent);
    location.hash = '#/prices';
    await sleep(500);
};

btn('+ тип цен').click();
await sleep(600);
expect([...document.querySelectorAll('.tabs button.active')].some(b => b.textContent === 'Вариант 1'), 'тип «Вариант 1» не создан или не выбран');
await setPrice('Вариант 1', '45,5');
await setPrice('Себестоимость', '31,20');

[...document.querySelectorAll('.tabs button')].find(b => b.textContent === 'Вариант 1').click();
await sleep(500);
const priceRow = [...document.querySelectorAll('tbody tr')].map(r => r.innerText.replace(/\s+/g, ' ').trim());
log.push('прайс «Вариант 1»: ' + priceRow.join(' | '));
expect(priceRow[0].startsWith('Батон 45,50 за шт'), 'в прайсе нет батона по 45,50');

// местная дата, как в программе: toISOString() дал бы UTC и после полуночи - вчерашний день
location.hash = '#/docs/new?type=production&date=' + isoDate(new Date());
await sleep(700);
const row = document.querySelector('tbody tr');
pick(row, 'Батон');
row.querySelector('input.qty').value = '40';
row.querySelector('input.qty').dispatchEvent(new Event('input'));
const cells = [...row.cells].map(c => c.textContent.replace(/\u00a0/g, ' ').trim());
log.push('строка поступления: ' + cells.filter(Boolean).join(' | '));
expect(cells.includes('31,20'), 'себестоимость не подставилась при выборе батона');
expect(cells.includes('1 248,00'), 'сумма себестоимости не посчиталась: 40 × 31,20');
const totals = document.querySelector('.totals').textContent.replace(/\u00a0/g, ' ');
expect(totals.includes('Себестоимость: 1 248,00'), 'итог себестоимости: ' + totals);
