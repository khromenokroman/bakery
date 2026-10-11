// Новое поступление: заполнить по двум заявкам, поправить количество, сохранить и провести,
// перепровести с другим количеством, отменить проведение.
btn('Заполнить по заявкам').click();
await sleep(500);
expect(document.querySelector('.panel').innerText.includes('№ 2'), 'в панели нет заявки № 2: ' + document.querySelector('.panel').innerText);
btn('Заполнить').click();
await sleep(500);
const rows = [...document.querySelectorAll('tbody tr')].map(r => r.querySelector('select').selectedOptions[0].text + ' ' + r.querySelector('input').value);
log.push('строки: ' + rows.join('; '));
expect(rows.join('; ') === 'Батон 45; Торт 5', 'ожидались строки «Батон 45; Торт 5»');
expect([...document.querySelectorAll('.fields label')].find(l => l.textContent.startsWith("Комментарий")).querySelector('input').value.startsWith('по заявкам № 1, 2'), 'комментарий не заполнен');

document.querySelector('tbody tr input').value = '44';
btn('Сохранить и провести').click();
await sleep(800);
log.push('после проведения: ' + document.querySelector('h2').innerText + ' / ' + location.hash);
expect(document.querySelector('.status.posted'), 'документ не проведён: ' + document.getElementById('message').textContent);
expect(/^#\/docs\/\d+$/.test(location.hash), 'адрес не документа: ' + location.hash);
let stock = await (await fetch('/api/stock')).json();
expect(stock.map(s => s.name + '=' + s.qty).join(', ') === 'Батон=44000, Торт=5000', 'остатки: ' + JSON.stringify(stock));

// перепроведение: исправили количество в проведённом документе
expect(btn('Сохранить и перепровести'), 'у проведённого документа нет кнопки «Сохранить и перепровести»');
document.querySelector('tbody tr input').value = '40';
btn('Сохранить и перепровести').click();
await sleep(800);
expect(document.getElementById('message').textContent === 'Сохранено и перепроведено', 'сообщение: ' + document.getElementById('message').textContent);
stock = await (await fetch('/api/stock')).json();
expect(stock.map(s => s.name + '=' + s.qty).join(', ') === 'Батон=40000, Торт=5000', 'остатки после перепроведения: ' + JSON.stringify(stock));
log.push('перепроведение: Батон 44 → 40');

btn('Отменить проведение').click();
await sleep(800);
expect(document.querySelector('.status.draft'), 'проведение не отменено');
stock = await (await fetch('/api/stock')).json();
expect(stock.length === 0, 'после отмены остались остатки: ' + JSON.stringify(stock));
log.push('отмена проведения: остатков нет');
