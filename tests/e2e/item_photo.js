// Фото позиции (#5): в карточке батона загрузить большой снимок (3000x2000) - он уменьшается до 1600 по большей
// стороне и уходит в JPEG; заменить маленьким PNG - уходит как есть; не картинка - понятная ошибка, фото прежнее;
// удалить фото с подтверждением.
const bread = (await (await fetch('/api/items')).json()).find(i => i.name === 'Батон').id;
const message = () => document.getElementById('message').textContent;
const picture = (w, h, type, name) => new Promise(resolve => {
    const canvas = document.createElement('canvas');
    canvas.width = w;
    canvas.height = h;
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#c96';
    ctx.fillRect(0, 0, w, h);
    canvas.toBlob(blob => resolve(new File([blob], name, {type})), type);
});
const choose = async file => {
    const input = document.querySelector('.photo input[type=file]');
    expect(input, 'в карточке нет выбора файла');
    const dt = new DataTransfer();
    dt.items.add(file);
    input.files = dt.files;
    input.dispatchEvent(new Event('change'));
    await sleep(1500);
};
const shown = async () => {
    const img = document.querySelector('.photo img');
    if (img && !img.complete) await new Promise(r => { img.onload = img.onerror = r; });
    return img;
};

location.hash = '#/items/' + bread;
await sleep(700);
expect(document.querySelector('.photo-empty'), 'у новой позиции должно быть «Фото нет»');
expect(btn('Загрузить фото'), 'нет кнопки «Загрузить фото»');

await choose(await picture(3000, 2000, 'image/png', 'big.png'));
expect(message().includes('Фото загружено'), 'нет сообщения о загрузке: ' + message());
let img = await shown();
expect(img && img.naturalWidth === 1600 && img.naturalHeight === 1067, 'большое фото должно уменьшиться до 1600x1067: ' + (img && img.naturalWidth + 'x' + img.naturalHeight));
let res = await fetch(`/api/items/${bread}/photo`);
expect(res.headers.get('Content-Type') === 'image/jpeg', 'большое фото должно уйти в JPEG: ' + res.headers.get('Content-Type'));
expect(btn('Заменить фото') && btn('Удалить фото'), 'после загрузки должны быть «Заменить фото» и «Удалить фото»');
log.push('3000x2000 PNG -> JPEG 1600x1067');

await choose(await picture(200, 100, 'image/png', 'small.png'));
img = await shown();
expect(img && img.naturalWidth === 200, 'маленькое фото должно заменить большое: ' + (img && img.naturalWidth));
res = await fetch(`/api/items/${bread}/photo`);
expect(res.headers.get('Content-Type') === 'image/png', 'маленький PNG должен уйти как есть: ' + res.headers.get('Content-Type'));
log.push('200x100 PNG заменил фото без пережатия');

await choose(new File(['не картинка'], 'notes.txt', {type: 'text/plain'}));
expect(message().includes('не удалось прочитать «notes.txt»'), 'нет ошибки для не картинки: ' + message());
expect(document.querySelector('.photo img'), 'после ошибки фото должно остаться');
log.push('текстовый файл отклонён: ' + message());

btn('Удалить фото').click();
await sleep(700);
expect(message().includes('Фото удалено') && document.querySelector('.photo-empty'), 'фото не удалилось: ' + message());
log.push('фото удалено');
