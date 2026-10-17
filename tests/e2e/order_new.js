// Новая заявка: выбрать продукцию, количество с запятой, сохранить; должен открыться список заявок с ней.
const row = document.querySelector('tbody tr');
const select = row.querySelector('select');
select.value = [...select.options].find(o => o.text === 'Батон').value;
select.dispatchEvent(new Event('change'));
row.querySelector('input').value = '3,5';
[...document.querySelectorAll('.fields label')].find(l => l.textContent.startsWith("Комментарий")).querySelector('input').value = 'Сидоров';
btn('Сохранить').click();
await sleep(800);
log.push('после сохранения: ' + location.hash + ' / ' + document.querySelector('h2').innerText);
expect(document.querySelector('h2').innerText === 'Заявки', 'не открылся список заявок');
const text = document.querySelector('tbody').innerText;
expect(text.includes('Сидоров') && text.includes('Батон 3,5 шт'), 'в списке нет новой заявки: ' + text);

// колонка позиций и кол-ва
const lastRow = document.querySelector('tbody tr:last-child');
const colsText = lastRow.querySelectorAll('td')[3].innerText;
expect(colsText.includes('1 /') && colsText.includes('3,5 шт'), 'нет формата позиции/кол-во: ' + colsText);
