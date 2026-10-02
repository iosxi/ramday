# ramday.ico を作る（python make-icon.py。Pillow が要る）
from PIL import Image, ImageDraw
def draw(n):
    S = 256
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 基板
    d.rounded_rectangle([12, 64, 244, 196], radius=14, fill=(32, 128, 84), outline=(18, 80, 52), width=6)
    # 切り欠き
    d.rectangle([118, 178, 138, 200], fill=(0, 0, 0, 0))
    # 金の端子
    for x in range(26, 236, 18):
        if 112 <= x <= 138: continue
        d.rectangle([x, 168, x + 10, 190], fill=(232, 186, 58))
    # チップ
    for x in (30, 92, 154):
        d.rounded_rectangle([x, 84, x + 52, 150], radius=6, fill=(30, 30, 36))
    # 伸び縮みの矢印（上）
    d.polygon([(128, 6), (168, 50), (88, 50)], fill=(64, 156, 255))
    im = im.resize((n, n), Image.LANCZOS)
    return im
sizes = [16, 20, 24, 32, 40, 48, 64, 256]
imgs = [draw(s) for s in sizes]
imgs[-1].save("ramday.ico", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
