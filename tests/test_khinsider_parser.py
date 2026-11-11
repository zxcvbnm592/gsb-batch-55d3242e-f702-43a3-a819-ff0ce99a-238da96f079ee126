import os

import pytest

from tunetape import khinsider
from tunetape.khinsider import _AlbumPageParser, _TrackPageParser
from tunetape.models import Track

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")


def _read(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as f:
        return f.read()


def test_album_parser_title_and_tracks():
    base = "https://downloads.khinsider.com/game-soundtracks/album/wii-console-background-music"
    parser = _AlbumPageParser(base)
    parser.feed(_read("album.html"))

    # First <h2> inside #pageContent wins; the earlier one is ignored.
    assert parser.title.strip() == "Wii Console Background Music"

    names = [name for _, name in parser.tracks]
    urls = [url for url, _ in parser.tracks]

    # Only same-album links are captured (the other-album decoy is excluded).
    assert len(parser.tracks) == 4
    assert "Title Theme" in names
    assert "Mii Channel" in names
    # Extension broadening: a .flac track must be picked up too.
    assert "Flac Track" in names
    assert any(u.endswith(".flac") for u in urls)
    # Empty anchor text -> name derived from URL filename (numbers/ext stripped).
    assert "instrumental" in names
    # All resolved to absolute khinsider URLs, deduped.
    assert all(u.startswith("https://downloads.khinsider.com") for u in urls)
    assert len(set(urls)) == len(urls)


def test_track_parser_skips_khinsider_and_finds_cdn_link():
    parser = _TrackPageParser()
    parser.feed(_read("track.html"))
    assert parser.direct_url == (
        "https://vgmsite.com/soundtracks/wii-console-background-music/title-theme.mp3"
    )


def test_track_parser_collects_candidates_deduped_keeping_first():
    # Several mirrors for the same track; the repeated third URL must collapse
    # onto its first occurrence and document order must be preserved.
    html = """
    <html><body><div id="pageContent">
      <a href="https://downloads.khinsider.com/game-soundtracks/album/x/01.mp3">album page</a>
      <a href="https://vgmsite.com/soundtracks/x/01-theme.mp3">mirror one</a>
      <a href="https://cdn.example.com/x/01-theme.flac">mirror two</a>
      <a href="https://vgmsite.com/soundtracks/x/01-theme.mp3">mirror one again</a>
      <a href="https://other.example.net/x/01-theme.ogg">mirror three</a>
    </div></body></html>
    """
    parser = _TrackPageParser()
    parser.feed(html)

    assert parser.candidates == [
        "https://vgmsite.com/soundtracks/x/01-theme.mp3",
        "https://cdn.example.com/x/01-theme.flac",
        "https://other.example.net/x/01-theme.ogg",
    ]
    # The khinsider page link is never treated as a CDN audio link...
    assert all("khinsider.com" not in url for url in parser.candidates)
    # ...and the preferred link is still the first candidate.
    assert parser.direct_url == parser.candidates[0]


def test_track_parser_accepts_query_strings_and_mixed_case_extensions():
    html = """
    <html><body>
      <a href="https://cdn.example.com/a/song.MP3?token=abc123&amp;expires=99">query + upper case</a>
      <a href="https://cdn.example.com/b/tune.FLAC">upper case extension</a>
      <a href="https://cdn.example.com/c/clip.OgG?x=1">mixed case + query</a>
      <a href="https://cdn.example.com/d/noise.txt">not an audio file</a>
      <a href="https://cdn.example.com/e/meta">no extension</a>
    </body></html>
    """
    parser = _TrackPageParser()
    parser.feed(html)

    assert parser.candidates == [
        "https://cdn.example.com/a/song.MP3?token=abc123&expires=99",
        "https://cdn.example.com/b/tune.FLAC",
        "https://cdn.example.com/c/clip.OgG?x=1",
    ]
    assert parser.direct_url == "https://cdn.example.com/a/song.MP3?token=abc123&expires=99"


def test_resolve_track_url_uses_first_candidate_and_caches(monkeypatch):
    page_url = "https://downloads.khinsider.com/game-soundtracks/album/x/01-song.mp3"
    html = """
    <html><body>
      <a href="https://vgmsite.com/soundtracks/x/first.mp3">first</a>
      <a href="https://cdn.example.com/x/second.flac">second</a>
    </body></html>
    """
    fetched = []

    def fake_fetch(url):
        fetched.append(url)
        return html

    monkeypatch.setattr(khinsider, "_fetch_page", fake_fetch)

    track = Track(name="Song", resolve_hint=page_url)
    first = khinsider.resolve_track_url(track)

    assert first == "https://vgmsite.com/soundtracks/x/first.mp3"
    # The chosen link is cached back onto the track...
    assert track.direct_url == first
    assert fetched == [page_url]

    # ...so a second resolution must not fetch the track page again.
    second = khinsider.resolve_track_url(track)
    assert second == first
    assert len(fetched) == 1


def test_resolve_track_url_no_candidates_names_track_and_page(monkeypatch):
    page_url = "https://downloads.khinsider.com/game-soundtracks/album/x/99-missing.mp3"
    # A page whose only links point back at khinsider pages: no CDN candidate.
    html = """
    <html><body><div id="pageContent">
      <a href="https://downloads.khinsider.com/game-soundtracks/album/x/99-missing.mp3">back</a>
      <a href="https://downloads.khinsider.com/game-soundtracks/album/x">album</a>
    </div></body></html>
    """
    monkeypatch.setattr(khinsider, "_fetch_page", lambda url: html)

    track = Track(name="Missing Song", resolve_hint=page_url)
    with pytest.raises(RuntimeError) as excinfo:
        khinsider.resolve_track_url(track)

    message = str(excinfo.value)
    # The error must let the user tell which track/page failed and why...
    assert "Missing Song" in message
    assert page_url in message
    # ...and mention the two plausible causes, without leaking any audio link.
    assert "format" in message or "changed" in message
    assert "no download" in message
    assert "vgmsite.com" not in message and "cdn.example.com" not in message
    # Nothing got cached when resolution failed.
    assert track.direct_url is None
