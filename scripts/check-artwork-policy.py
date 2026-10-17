"""Isolated artwork policy tests; Worker and official catalogue calls are mocked."""
import unittest
from unittest.mock import patch
import json
from pathlib import Path
from nintendo_tw_artwork import OFFICIAL, preferred_sources, playstation_url
import preview_game_bridge as bridge

COMMUNITY={'id':1,'url':'https://cdn2.steamgriddb.com/grid/72654158f52bf25f9a8101f78aceaa2f.jpg','language':None}
WORKER={'status':'ok','data':[COMMUNITY],'page':0,'hasMore':False}

class ArtworkPolicyTests(unittest.TestCase):
    @patch.object(bridge.HK_STORE,'search',side_effect=AssertionError('Hong Kong must not be queried'))
    @patch.object(bridge,'fetch_json',return_value=WORKER)
    @patch.object(bridge.TW_LINEUP,'search',side_effect=AssertionError('Worker-only covers must not query the official directory'))
    def test_worker_covers_do_not_use_static_nintendo_mappings(self,taiwan,worker,store):
        reply=bridge.covers(5267007,'all','zh-TW',0,'all','','switch')
        self.assertEqual(reply['artworkSources'],[])
        self.assertEqual([cover['provider'] for cover in reply['covers']],['steamgriddb'])
        self.assertEqual(reply['officialProvider'],'')
        store.assert_not_called()
        taiwan.assert_not_called()
        square=bridge.covers(5267007,'square','zh-TW',0,'all','','switch')
        self.assertEqual(square['artworkSources'],[])
        self.assertEqual([cover['provider'] for cover in square['covers']],['steamgriddb'])

    @patch.object(bridge.HK_STORE,'search',side_effect=AssertionError('Other platforms must not query Nintendo'))
    @patch.object(bridge,'fetch_json',return_value=WORKER)
    def test_platform_isolation(self,worker,store):
        for platform in ('playstation','other','unknown'):
            with self.subTest(platform=platform):
                reply=bridge.covers(5267007,'all','zh-TW',0,'all','Zelda',platform)
                self.assertEqual(reply['artworkSources'],[])
                self.assertEqual(len(reply['covers']),1)
        sony=bridge.covers(5454475,'all','zh-TW',0,'all','Astro Bot','playstation')
        self.assertEqual([cover['provider'] for cover in sony['artworkSources']],['playstation-tw'])
        self.assertEqual(sony['artworkSources'][0]['language'],'en-US')
        self.assertEqual(sony['artworkSources'][0]['shape'],'square')

    @patch.object(bridge,'fetch_json',return_value={'status':'ok','data':[],'page':0,'hasMore':False})
    def test_hong_kong_fallback(self,worker):
        with patch.object(bridge.TW_LINEUP,'search',return_value={'status':'empty','covers':[],'page':1,'hasMore':False,'total':0,'officialProvider':'nintendo-tw'}):
            candidate={'id':4200000999,'provider':'nintendo-hk-store','shape':'landscape','productTitle':'曠野之息'}
            with patch.object(bridge.HK_STORE,'search',return_value={'status':'ok','covers':[candidate],'page':1,'hasMore':True,'total':40}) as store:
                reply=bridge.covers(35744,'all','zh-TW',1,'nintendo','曠野之息','switch')
                self.assertEqual(reply['artworkSources'],[candidate])
                self.assertTrue(reply['hasMore'])
                self.assertEqual(reply['page'],1)
                self.assertEqual(reply['officialProvider'],'nintendo-hk-store')
                store.assert_called_once_with('曠野之息',1)

    @patch.object(bridge,'fetch_json',return_value={'status':'ok','data':[],'page':0,'hasMore':False})
    def test_taiwan_official_browse_and_locale_independence(self,worker):
        candidate={'id':4200001998,'provider':'nintendo-tw','sourceLabel':'Official Nintendo Taiwan','region':'TW','isOfficial':True,
                   'kind':'logo','shape':'landscape','url':'https://images.ctfassets.net/o5v89n4kg6h4/7001000013323143e1afaeac8b11f18660ebc15686256c/ff9b9a2c673b031deefe23e14af7e66c/b34d2cdc3942da8386bfba51c7f46fcf027f5a09901680b1c5a02d46cf585c51.jpg?w=400&fm=webp'}
        result={'status':'ok','covers':[candidate],'page':0,'hasMore':False,'total':1,'officialProvider':'nintendo-tw'}
        with patch.object(bridge.TW_LINEUP,'search',return_value=result) as taiwan, patch.object(bridge.HK_STORE,'search',side_effect=AssertionError('Taiwan results must be primary')):
            replies=[bridge.covers(5267007,'all',locale,0,'nintendo','ZELDA無雙 災厄啟示錄','switch') for locale in ('zh-TW','en-US','de-DE')]
            self.assertEqual([reply['officialProvider'] for reply in replies],['nintendo-tw']*3)
            self.assertEqual([[cover['url'] for cover in reply['artworkSources']] for reply in replies],[[candidate['url']]]*3)
            taiwan.assert_has_calls([unittest.mock.call('ZELDA無雙 災厄啟示錄',0)]*3)

    def test_taiwan_image_allowlist(self):
        valid='https://images.ctfassets.net/o5v89n4kg6h4/7001000013323143e1afaeac8b11f18660ebc15686256c/ff9b9a2c673b031deefe23e14af7e66c/b34d2cdc3942da8386bfba51c7f46fcf027f5a09901680b1c5a02d46cf585c51.jpg?w=400&fm=webp'
        self.assertTrue(bridge.TW_LINEUP.image_url(valid))
        self.assertFalse(bridge.TW_LINEUP.image_url(valid.replace('images.ctfassets.net','images.ctfassets.net.evil.test')))
        self.assertFalse(bridge.TW_LINEUP.image_url(valid.replace('w=400&fm=webp','w=800&fm=webp')))

    @patch.object(bridge.TW_LINEUP,'search')
    @patch.object(bridge,'fetch_json')
    def test_live_directory_match_needs_no_game_id_mapping(self,worker,taiwan):
        worker.return_value={'status':'ok','data':[{'id':5303117,'name':'Worker Game','displayName':'Worker Game'}],
                             'lookups':{},'resolvedQueries':[],'total':1}
        candidate={'id':4200001999,'provider':'nintendo-tw','sourceLabel':'Official Nintendo Taiwan','region':'TW','isOfficial':True,
                   'kind':'logo','shape':'landscape','url':'https://images.ctfassets.net/o5v89n4kg6h4/7001000013323143e1afaeac8b11f18660ebc15686256c/ff9b9a2c673b031deefe23e14af7e66c/b34d2cdc3942da8386bfba51c7f46fcf027f5a09901680b1c5a02d46cf585c51.jpg?w=400&fm=webp',
                   'productTitle':'Taiwan Product'}
        taiwan.return_value={'status':'ok','covers':[candidate]}
        reply=bridge.search('Taiwan Product','de-DE','switch')
        self.assertEqual(reply['workerCount'],1)
        self.assertEqual(reply['games'][0]['id'],5303117)
        self.assertEqual(reply['games'][0]['artworkSources'][0]['url'],candidate['url'])
        self.assertEqual(reply['games'][0]['artworkSources'][0]['provider'],'nintendo-tw')

    def test_nintendo_catalog_is_dynamic_and_playstation_seed_remains(self):
        self.assertEqual(OFFICIAL.candidates(5303117,'switch'),[])
        self.assertEqual(OFFICIAL.candidates(5267007,'switch'),[])
        self.assertEqual(OFFICIAL.merge([],'王國之淚','zh-TW','switch'),[])
        sony=OFFICIAL.merge([],'Astro Bot','zh-TW','playstation')
        self.assertEqual(sony[0]['source'],'playstation-tw')
        self.assertEqual(OFFICIAL.merge([],'Astro Bot','zh-TW','switch'),[])
        self.assertEqual(preferred_sources(sony[0]['artworkSources'],'unknown'),[])
        self.assertFalse(playstation_url('https://image.api.playstation.com.evil.test/vulcan/a.png'))

    def test_nintendo_game_search_uses_worker_without_title_expansions(self):
        expansions=json.loads((Path(__file__).resolve().parents[1]/'game-search-expansions.json').read_text(encoding='utf-8'))
        entries=[value for group in expansions.get('groups',[]) for field in ('aliases','queries','suggestions') for value in group.get(field,[])]
        self.assertFalse(any('zelda' in value.casefold() or 'hyrule' in value.casefold() for value in entries))

if __name__=='__main__':unittest.main(verbosity=2)
