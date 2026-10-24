# Archive fixtures

`ArchiveFixture.hpp` generates ROM archives during each test, including a stored RAR 4 image. No external archiver or game download is required.

`compressed-rar5.rar` is the decoded `test_read_format_rar5_compressed.rar.uu` fixture from libarchive 3.8.10. It contains 1200 bytes of generated integer data and exercises compressed RAR 5 decoding. Its license is in [RAR-LICENSE.txt](RAR-LICENSE.txt).
