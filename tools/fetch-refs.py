#!/usr/bin/env python3
# Reference photos for modelling the landmarks: downloads small (500 px)
# versions of the Wikimedia Commons photos in a category or matching a search,
# and writes refs.json with each photo's author and licence. The photos are
# only looked at while modelling and painting textures; they are not used in
# the game.
#
#   python3 tools/fetch-refs.py <out dir> --category "Muradie Mosque" [--max 12]
#   python3 tools/fetch-refs.py <out dir> --search "Petro Marko theatre Vlore"
#
# Wikimedia rate-limits image downloads: this waits and retries on 429.
import argparse, html, json, os, re, sys, time, urllib.parse
import requests

UA = 'psloje-landmarks/0.1 (https://github.com/sandrinsol/psloje)'
API = 'https://commons.wikimedia.org/w/api.php'
ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--category')
ap.add_argument('--search')
ap.add_argument('--max', type=int, default=12)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
s = requests.Session()
s.headers['User-Agent'] = UA

def get(url, **kw):
    for attempt in range(8):
        r = s.get(url, timeout=60, **kw)
        if r.status_code == 429 or r.status_code >= 500:
            time.sleep(5 * (attempt + 1))
            continue
        return r
    return r

params = {'action': 'query', 'format': 'json', 'prop': 'imageinfo', 'iiprop': 'url|extmetadata', 'iiurlwidth': 500}
if a.category:
    params.update(generator='categorymembers', gcmtitle='Category:' + a.category, gcmtype='file', gcmlimit=50)
else:
    params.update(generator='search', gsrsearch=a.search, gsrnamespace=6, gsrlimit=30)
pages = (get(API, params=params).json().get('query') or {}).get('pages', {})
strip = lambda v: html.unescape(re.sub(r'<[^>]+>', '', v or '')).strip()
refs = []
for p in sorted(pages.values(), key=lambda p: p['title']):
    if len(refs) >= a.max:
        break
    ii = p['imageinfo'][0]
    if not re.search(r'\.(jpe?g|png)$', p['title'], re.I):
        continue
    # Thumbnails from upload.wikimedia.org (the standard 500 px size).
    url = ii.get('thumburl', ii['url']).replace('://thumb.wikimedia.org/wikipedia/', '://upload.wikimedia.org/wikipedia/').split('?')[0]
    name = f'{len(refs) + 1:02d}.jpg'
    r = get(url)
    if r.status_code != 200 or not r.headers.get('content-type', '').startswith('image/'):
        print(f'skip {p["title"]}: {r.status_code}', file=sys.stderr)
        continue
    open(os.path.join(a.out, name), 'wb').write(r.content)
    m = ii.get('extmetadata', {})
    refs.append({'file': name, 'title': p['title'], 'page': ii.get('descriptionurl'),
                 'author': strip(m.get('Artist', {}).get('value')), 'licence': strip(m.get('LicenseShortName', {}).get('value'))})
    time.sleep(3)
json.dump(refs, open(os.path.join(a.out, 'refs.json'), 'w'), indent=1, ensure_ascii=False)
print(f'{a.out}: {len(refs)} photos', file=sys.stderr)
