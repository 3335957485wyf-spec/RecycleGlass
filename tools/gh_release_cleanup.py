# Remove the intermediate bug-fix releases (and their tags) from GitHub so the
# Releases page only lists meaningful versions.  Credentials come from the git
# credential helper, never printed.
import subprocess, json, urllib.request, sys

REPO = "3335957485wyf-spec/RecycleGlass"
DROP = ["v4.11.2", "v4.11.3", "v4.11.4"]


def token():
    out = subprocess.run(["git", "credential", "fill"],
                         input="protocol=https\nhost=github.com\n\n",
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    raise SystemExit("no github token in the credential helper")


T = token()


def api(path, method="GET"):
    req = urllib.request.Request("https://api.github.com" + path, method=method,
                                 headers={"Authorization": "Bearer " + T,
                                          "Accept": "application/vnd.github+json",
                                          "User-Agent": "rg-release-tool"})
    try:
        with urllib.request.urlopen(req, timeout=40) as r:
            body = r.read().decode("utf-8")
            return r.status, (json.loads(body) if body.strip() else None)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "ignore")[:200]


st, rel = api(f"/repos/{REPO}/releases?per_page=30")
if st != 200:
    raise SystemExit(f"list failed: {st} {rel}")
by_tag = {x["tag_name"]: x for x in rel}

for tag in DROP:
    r = by_tag.get(tag)
    if not r:
        print(f"{tag}: no release")
    else:
        s, body = api(f"/repos/{REPO}/releases/{r['id']}", "DELETE")
        print(f"{tag}: release delete -> {s}")

st, refs = api(f"/repos/{REPO}/git/refs/tags")
if st == 200:
    have = {x["ref"] for x in refs}
    for tag in DROP:
        ref = f"refs/tags/{tag}"
        if ref in have:
            s, body = api(f"/repos/{REPO}/git/{ref}", "DELETE")
            print(f"{tag}: tag delete -> {s}")
        else:
            print(f"{tag}: tag already gone")
else:
    print("tag list failed", st, refs)

st, rel = api(f"/repos/{REPO}/releases?per_page=30")
print("\nremaining releases:")
for x in rel:
    print("  ", x["tag_name"], "|", x["name"])
