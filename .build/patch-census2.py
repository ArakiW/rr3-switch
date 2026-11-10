import io
p = r"E:\AI\RealRacing3-switch\tools\Test-OnEden.ps1"
text = io.open(p, encoding="utf-8", errors="surrogateescape").read()
old = """    $portGuiOpens = [regex]::Matches($guestText, ' gui=(\\d+) ')
    $portTwosquare = [regex]::Matches($guestText, ' twosq=(\\d+) ')"""
new = """    # The LAST field of the line has no trailing space, so the pattern must not
    # demand one: ' twosq=(\\d+)(?:\\s|$)'.
    $portGuiOpens = [regex]::Matches($guestText, ' gui=(\\d+) ')
    $portTwosquare = [regex]::Matches($guestText, ' twosq=(\\d+)(?:\\s|$)')"""
if old in text:
    text = text.replace(old, new)
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
    print("census regex fixed")
else:
    print("PATTERN NOT FOUND")