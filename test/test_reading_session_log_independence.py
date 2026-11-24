from pathlib import Path
import unittest


class ReadingSessionLogIndependenceTest(unittest.TestCase):
    def test_native_log_is_not_gated_by_plugin_subscription(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'src/activities/reader/ReaderActivity.cpp').read_text(encoding='utf-8')
        flush = source.split('void ReaderActivity::flushReaderSession() {', 1)[1].split('\nbool ReaderActivity::', 1)[0]
        before_log, after_log = flush.split('readingSessionLog::append(', 1)
        self.assertIn('if (!readerSession.isEmitWorthy()) {', before_log)
        self.assertNotIn('!pluginevents::anySubscriber', before_log)
        self.assertNotIn('if (subscribed', before_log)
        self.assertIn('if (subscribed && validDocument)', after_log)
        self.assertIn('pluginevents::emit(', after_log)
        self.assertLess(before_log.index('readingstats::record('), before_log.index('if (!readerSession.isEmitWorthy())'))


if __name__ == '__main__':
    unittest.main()
