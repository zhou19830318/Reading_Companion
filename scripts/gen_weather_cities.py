#!/usr/bin/env python3
"""Generate lib/WeatherCities/WeatherCities.inc from open-meteo geocoding.

Input: RAW, one `english|中文名|省份` triple per line (244 prefecture-level
cities). Output: one `{"中文名", "english", lat, lon},` row per city, sorted
pinyin-wise by the english key.

Why not the IP lookup instead: free IP geolocation puts this device (Changzhou,
Jiangsu Telecom) in Nanjing or returns coordinates in the wrong province (ip.sb
IPv4 -> 34.77,113.72, i.e. Henan). A compiled-in table plus one user choice
gives a fixed, correct location and skips the geo HTTP step entirely.

Matching notes (learned the hard way -- see the doc comment in WeatherCities.h):
  * query by the ENGLISH name: open-meteo's zh search misses many Chinese names
    (常州/抚州/台北 return 0 hits) while `name=Changzhou&language=zh` returns
    the right Chinese label.
  * admin1 comes back inconsistently (`江苏` and `江苏省` both occur), so the
    province is normalised on both sides before comparing.
  * HK/Macau/Taiwan have no usable admin1 (and Taiwan's is written in
    traditional script), so those three are pinned to a country code.
  * only PPL* features are candidates: airports/helicopter pads share city
    names (杭州萧山国际机场).

Run: python3 scripts/gen_weather_cities.py
"""
import json, time, urllib.parse, urllib.request
from concurrent.futures import ThreadPoolExecutor

OUT = 'lib/WeatherCities/WeatherCities.inc'

EXPECTED_CC = {'台湾': 'TW', '香港': 'HK', '澳门': 'MO'}
PROV_SUFFIXES = ('壮族自治区', '回族自治区', '维吾尔自治区', '特别行政区', '自治区', '省', '市')

RAW = """
Beijing|北京|北京市
Shanghai|上海|上海市
Tianjin|天津|天津市
Chongqing|重庆|重庆市
Shijiazhuang|石家庄|河北省
Taiyuan|太原|山西省
Hohhot|呼和浩特|内蒙古
Shenyang|沈阳|辽宁省
Changchun|长春|吉林省
Harbin|哈尔滨|黑龙江省
Hangzhou|杭州|浙江省
Hefei|合肥|安徽省
Fuzhou|福州|福建省
Nanchang|南昌|江西省
Jinan|济南|山东省
Zhengzhou|郑州|河南省
Wuhan|武汉|湖北省
Changsha|长沙|湖南省
Guangzhou|广州|广东省
Nanning|南宁|广西
Haikou|海口|海南省
Chengdu|成都|四川省
Guiyang|贵阳|贵州省
Kunming|昆明|云南省
Lhasa|拉萨|西藏
Xian|西安|陕西省
Lanzhou|兰州|甘肃省
Xining|西宁|青海省
Yinchuan|银川|宁夏
Urumqi|乌鲁木齐|新疆
Hong Kong|香港|香港
Macau|澳门|澳门
Taipei|台北|台湾
Taichung|台中|台湾
Tainan|台南|台湾
Kaohsiung|高雄|台湾
Nanjing|南京|江苏省
Wuxi|无锡|江苏省
Xuzhou|徐州|江苏省
Changzhou|常州|江苏省
Suzhou|苏州|江苏省
Nantong|南通|江苏省
Yangzhou|扬州|江苏省
Zhenjiang|镇江|江苏省
Taizhou|泰州|江苏省
Yancheng|盐城|江苏省
Huaian|淮安|江苏省
Lianyungang|连云港|江苏省
Suqian|宿迁|江苏省
Ningbo|宁波|浙江省
Wenzhou|温州|浙江省
Jiaxing|嘉兴|浙江省
Huzhou|湖州|浙江省
Shaoxing|绍兴|浙江省
Jinhua|金华|浙江省
Quzhou|衢州|浙江省
Zhoushan|舟山|浙江省
Taizhou|台州|浙江省
Lishui|丽水|浙江省
Qingdao|青岛|山东省
Zibo|淄博|山东省
Zaozhuang|枣庄|山东省
Dongying|东营|山东省
Yantai|烟台|山东省
Weifang|潍坊|山东省
Jining|济宁|山东省
Taian|泰安|山东省
Weihai|威海|山东省
Rizhao|日照|山东省
Linyi|临沂|山东省
Dezhou|德州|山东省
Liaocheng|聊城|山东省
Binzhou|滨州|山东省
Heze|菏泽|山东省
Shenzhen|深圳|广东省
Zhuhai|珠海|广东省
Shantou|汕头|广东省
Foshan|佛山|广东省
Jiangmen|江门|广东省
Zhanjiang|湛江|广东省
Maoming|茂名|广东省
Zhaoqing|肇庆|广东省
Huizhou|惠州|广东省
Meizhou|梅州|广东省
Heyuan|河源|广东省
Yangjiang|阳江|广东省
Qingyuan|清远|广东省
Dongguan|东莞|广东省
Zhongshan|中山|广东省
Chaozhou|潮州|广东省
Jieyang|揭阳|广东省
Yunfu|云浮|广东省
Shanwei|汕尾|广东省
Zigong|自贡|四川省
Panzhihua|攀枝花|四川省
Luzhou|泸州|四川省
Deyang|德阳|四川省
Mianyang|绵阳|四川省
Guangyuan|广元|四川省
Suining|遂宁|四川省
Neijiang|内江|四川省
Leshan|乐山|四川省
Nanchong|南充|四川省
Meishan|眉山|四川省
Yibin|宜宾|四川省
Guangan|广安|四川省
Dazhou|达州|四川省
Ya'an|雅安|四川省
Bazhong|巴中|四川省
Ziyang|资阳|四川省
Yichang|宜昌|湖北省
Xiangyang|襄阳|湖北省
Jingzhou|荆州|湖北省
Huanggang|黄冈|湖北省
Shiyan|十堰|湖北省
Xiaogan|孝感|湖北省
Jingmen|荆门|湖北省
Ezhou|鄂州|湖北省
Huangshi|黄石|湖北省
Xianning|咸宁|湖北省
Suizhou|随州|湖北省
Zhuzhou|株洲|湖南省
Xiangtan|湘潭|湖南省
Hengyang|衡阳|湖南省
Shaoyang|邵阳|湖南省
Yueyang|岳阳|湖南省
Changde|常德|湖南省
Yiyang|益阳|湖南省
Chenzhou|郴州|湖南省
Yongzhou|永州|湖南省
Huaihua|怀化|湖南省
Loudi|娄底|湖南省
Wuhu|芜湖|安徽省
Bengbu|蚌埠|安徽省
Huainan|淮南|安徽省
Maanshan|马鞍山|安徽省
Huaibei|淮北|安徽省
Tongling|铜陵|安徽省
Anqing|安庆|安徽省
Huangshan|黄山|安徽省
Chuzhou|滁州|安徽省
Fuyang|阜阳|安徽省
Suzhou|宿州|安徽省
Luan|六安|安徽省
Bozhou|亳州|安徽省
Chizhou|池州|安徽省
Xuancheng|宣城|安徽省
Ganzhou|赣州|江西省
Jiujiang|九江|江西省
Shangrao|上饶|江西省
Yichun|宜春|江西省
Jian|吉安|江西省
Fuzhou|抚州|江西省
Jingdezhen|景德镇|江西省
Pingxiang|萍乡|江西省
Xinyu|新余|江西省
Yingtan|鹰潭|江西省
Xiamen|厦门|福建省
Quanzhou|泉州|福建省
Zhangzhou|漳州|福建省
Putian|莆田|福建省
Sanming|三明|福建省
Nanping|南平|福建省
Longyan|龙岩|福建省
Ningde|宁德|福建省
Qinhuangdao|秦皇岛|河北省
Tangshan|唐山|河北省
Handan|邯郸|河北省
Xingtai|邢台|河北省
Baoding|保定|河北省
Zhangjiakou|张家口|河北省
Chengde|承德|河北省
Cangzhou|沧州|河北省
Langfang|廊坊|河北省
Hengshui|衡水|河北省
Datong|大同|山西省
Yangquan|阳泉|山西省
Changzhi|长治|山西省
Jincheng|晋城|山西省
Shuozhou|朔州|山西省
Jinzhong|晋中|山西省
Yuncheng|运城|山西省
Xinzhou|忻州|山西省
Linfen|临汾|山西省
Luliang|吕梁|山西省
Anshan|鞍山|辽宁省
Fushun|抚顺|辽宁省
Benxi|本溪|辽宁省
Dandong|丹东|辽宁省
Jinzhou|锦州|辽宁省
Yingkou|营口|辽宁省
Fuxin|阜新|辽宁省
Panjin|盘锦|辽宁省
Tieling|铁岭|辽宁省
Chaoyang|朝阳|辽宁省
Jilin|吉林|吉林省
Siping|四平|吉林省
Liaoyuan|辽源|吉林省
Tonghua|通化|吉林省
Baicheng|白城|吉林省
Songyuan|松原|吉林省
Baishan|白山|吉林省
Qiqihar|齐齐哈尔|黑龙江省
Mudanjiang|牡丹江|黑龙江省
Jiamusi|佳木斯|黑龙江省
Daqing|大庆|黑龙江省
Yichun|伊春|黑龙江省
Qitaihe|七台河|黑龙江省
Hegang|鹤岗|黑龙江省
Shuangyashan|双鸭山|黑龙江省
Suihua|绥化|黑龙江省
Heihe|黑河|黑龙江省
Chifeng|赤峰|内蒙古
Hailar|呼伦贝尔|内蒙古
Tongliao|通辽|内蒙古
Baotou|包头|内蒙古
Ordos|鄂尔多斯|内蒙古
Wuhai|乌海|内蒙古
Yulin|榆林|陕西省
Baoji|宝鸡|陕西省
Xianyang|咸阳|陕西省
Weinan|渭南|陕西省
YanAn|延安|陕西省
Hanzhong|汉中|陕西省
Ankang|安康|陕西省
Shangluo|商洛|陕西省
Tongchuan|铜川|陕西省
Luoyang|洛阳|河南省
Kaifeng|开封|河南省
Anyang|安阳|河南省
Hebi|鹤壁|河南省
Xinxiang|新乡|河南省
Jiaozuo|焦作|河南省
Puyang|濮阳|河南省
Xuchang|许昌|河南省
Luohe|漯河|河南省
Sanmenxia|三门峡|河南省
Nanyang|南阳|河南省
Shangqiu|商丘|河南省
Xinyang|信阳|河南省
Zhoukou|周口|河南省
Zhumadian|驻马店|河南省
Pingdingshan|平顶山|河南省
Jiyuan|济源|河南省
"""


def norm_prov(p):
    for suf in PROV_SUFFIXES:
        if p.endswith(suf):
            return p[:-len(suf)]
    return p


def query(name):
    q = urllib.parse.urlencode({'name': name, 'count': 20, 'language': 'zh', 'format': 'json'})
    url = 'https://geocoding-api.open-meteo.com/v1/search?' + q
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=20) as r:
                return json.load(r).get('results') or []
        except Exception:
            if attempt == 2:
                return []
            time.sleep(1.0 + attempt)
    return []


def score(res, en, zh, prov):
    if not (res.get('feature_code') or '').startswith('PPL'):
        return None
    cc = res.get('country_code')
    if prov in EXPECTED_CC:
        if cc != EXPECTED_CC[prov]:
            return None
    else:
        if cc != 'CN':
            return None
        if norm_prov(res.get('admin1') or '') != norm_prov(prov):
            return None
    name = res.get('name') or ''
    s = 1
    if name == zh or name == zh + '市':
        s += 10 ** 9
    elif zh in name or name in zh:
        s += 10 ** 8
    s += res.get('population') or 0
    if (res.get('feature_code') or '') in ('PPLC', 'PPLA', 'PPLA1', 'PPLA2', 'PPLA3'):
        s += 10 ** 7
    return s


def photon(zh, prov):
    # open-meteo's index has no entry for a few prefecture cities (泰安/广安/
    # 马鞍山/辽源 under any spelling), so fall back to Photon, the hosted OSM
    # geocoder: same data as Nominatim (which is unreachable from here), no key.
    # Query the full "city, province, country" string and re-check the state
    # below — Photon ranks on free text, not on admin boundaries.
    q = urllib.parse.urlencode({'q': '%s市, %s, 中国' % (zh, prov), 'limit': 5})
    url = 'https://photon.komoot.io/api/?' + q
    try:
        with urllib.request.urlopen(url, timeout=20) as r:
            feats = json.load(r).get('features') or []
    except Exception:
        return None
    for feat in feats:
        props = feat.get('properties') or {}
        if props.get('countrycode') != 'CN' or props.get('osm_key') != 'place':
            continue
        if norm_prov(props.get('state') or '') != norm_prov(prov):
            continue
        lon, lat = feat['geometry']['coordinates']
        return {'name': zh, 'latitude': lat, 'longitude': lon, 'feature_code': 'PPL',
                'country_code': 'CN', 'admin1': props.get('state'), 'population': 0}
    return None


def fetch(en, zh, prov):
    # Both spellings are queried (not `en or zh`): open-meteo answers 常州 only
    # for "Changzhou" and 六安 only for "六安", so one pass each way is needed.
    cands = []
    for res in query(en) + query(zh):
        sc = score(res, en, zh, prov)
        if sc is not None:
            cands.append((sc, res))
    if cands:
        cands.sort(key=lambda t: -t[0])
        return cands[0][1]
    return photon(zh, prov)


rows, failed = [], []


def process(line):
    parts = line.strip().split('|')
    if len(parts) != 3:
        return ('bad', line, 'malformed')
    en, zh, prov = parts
    res = fetch(en, zh, prov)
    if res is None:
        return ('bad', line, 'no candidate')
    return ('ok', {'en': en, 'zh': zh, 'lat': res['latitude'], 'lon': res['longitude']}, None)


lines = [l for l in RAW.strip().split('\n') if l.strip()]
# 488 sequential lookups (EN + ZH pass each) ran past 7 minutes; 8 workers keeps
# the whole table rebuild under two and neither endpoint minds the rate.
with ThreadPoolExecutor(max_workers=8) as pool:
    for i, (kind, a, b) in enumerate(pool.map(process, lines), 1):
        if kind == 'ok':
            rows.append(a)
        else:
            failed.append((a, b))
        if i % 40 == 0:
            print('  %d/%d' % (i, len(lines)), flush=True)

rows.sort(key=lambda r: (r['en'].lower(), r['zh']))
keys = [r['zh'] for r in rows]
assert len(keys) == len(set(keys)), 'duplicate zh key: %s' % [k for k in keys if keys.count(k) > 1]

with open(OUT, 'w', encoding='utf-8') as f:
    for r in rows:
        f.write('    {"%s", "%s", %.4ff, %.4ff},\n' % (r['zh'], r['en'], r['lat'], r['lon']))
print('wrote %s: %d rows, %d failed' % (OUT, len(rows), len(failed)))
for line, why in failed:
    print('FAILED %s (%s)' % (line, why))
