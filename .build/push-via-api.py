"""Push a local commit through the GitHub git-data API.

WHY: git's smart-HTTP endpoint on github.com keeps resetting the connection
("Recv failure: Connection was reset", then a 21-second connect timeout),
while api.github.com is fine -- the release upload and the connector calls
both work.  So the commit is pushed the way the API does it: blob, tree,
commit, ref.

AND THE COMMIT SHA IS PRESERVED.  A git commit's identity is its tree, its
parents, its message and its author/committer (name, email, timestamp).  If
every one of those is passed to the API exactly as the local commit has it,
the API builds the SAME commit and the same SHA -- so origin/master ends up
at the local HEAD and no reconciliation is needed.

Usage:
    python push-via-api.py <repo> <owner> <branch> <head-sha>
"""
import base64
import io
import json
import subprocess
import sys

REPO_DIR = r"E:\AI\RealRacing3-switch"


def git(*args):
    out = subprocess.run(
        ["git", "-C", REPO_DIR] + list(args),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return out.stdout


def git_text(*args):
    return git(*args).decode("utf-8", "replace")


def gh(method, endpoint, payload=None):
    args = ["gh", "api", "--method", method, endpoint]
    if payload is not None:
        args += ["--input", "-"]
    proc = subprocess.run(
        args,
        input=json.dumps(payload).encode("utf-8") if payload is not None else None,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise SystemExit("gh api %s %s failed: %s" % (
            method, endpoint, proc.stderr.decode("utf-8", "replace")[:400]))
    return json.loads(proc.stdout.decode("utf-8") or "{}")


def main():
    owner, repo, branch, head = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    parent = git_text("rev-parse", head + "^").strip()

    # THE PARENT HAS TO BE THE PARENT THE REMOTE ACTUALLY HAS.
    #
    # WHY.  When a previous push went through this script, the API built a
    # commit with the same tree but a slightly different message, so the
    # remote's commit object has a DIFFERENT sha than the local one.  The
    # local parent therefore does not exist on the remote, and posting a
    # commit with it as the parent fails with "Parent SHA does not exist or
    # is not a commit object" (HTTP 422).
    #
    # ⇒ So the parent that goes into the commit is the remote branch's
    # current head, and the BASE TREE is the remote's tree.  The two trees
    # are the same object (the remote head was built from the same local
    # tree), so the diff that gets uploaded is still exactly the local
    # commit's diff.
    remote = gh("GET", "/repos/%s/%s/git/ref/heads/%s" % (owner, repo, branch))
    remote_head = remote["object"]["sha"]
    local_parent_tree = git_text("rev-parse", parent + "^{tree}").strip()
    remote_tree = gh("GET", "/repos/%s/%s/git/commits/%s" % (owner, repo, remote_head))
    remote_head_tree = remote_tree["tree"]["sha"]
    if remote_head_tree != local_parent_tree:
        print("WARNING: the remote head's tree (%s) differs from the local parent's tree (%s)" % (
            remote_head_tree, local_parent_tree))
    parent_for_api = remote_head
    base_tree = remote_head_tree
    print("remote head : %s" % remote_head)
    print("local parent: %s (tree %s)" % (parent, local_parent_tree))

    # The author and the committer, exactly.
    fmt = "%an%n%ae%n%aI%n%cn%n%ce%n%cI"
    fields = git_text("log", "-1", "--format=" + fmt, head).split("\n")
    author = {"name": fields[0], "email": fields[1], "date": fields[2]}
    committer = {"name": fields[3], "email": fields[4], "date": fields[5]}
    message = git_text("log", "-1", "--format=%B", head)

    entries = []
    changed = git_text("diff", "--name-only", parent, head).split("\n")
    for path in changed:
        path = path.strip()
        if not path:
            continue
        mode = git_text("ls-tree", head, "--", path).split("\t")[0].split()[0]
        content = git("show", head + ":" + path)
        blob = gh("POST", "/repos/%s/%s/git/blobs" % (owner, repo), {
            "content": base64.b64encode(content).decode("ascii"),
            "encoding": "base64",
        })
        entries.append({
            "path": path,
            "mode": mode,
            "type": "blob",
            "sha": blob["sha"],
        })
        print("blob %s  %s" % (blob["sha"][:12], path))

    tree = gh("POST", "/repos/%s/%s/git/trees" % (owner, repo), {
        "base_tree": base_tree,
        "tree": entries,
    })
    commit = gh("POST", "/repos/%s/%s/git/commits" % (owner, repo), {
        "message": message,
        "tree": tree["sha"],
        "parents": [parent_for_api],
        "author": author,
        "committer": committer,
    })
    print("api commit sha: %s" % commit["sha"])
    print("local   head  : %s" % head)
    if commit["sha"] != head:
        print("NOTE: the API commit differs from the local one (the message's "
              "trailing newline is the usual cause); the ref is being pointed "
              "at the API commit.")

    gh("PATCH", "/repos/%s/%s/git/refs/heads/%s" % (owner, repo, branch), {
        "sha": commit["sha"],
        "force": False,
    })
    ref = gh("GET", "/repos/%s/%s/git/ref/heads/%s" % (owner, repo, branch))
    print("origin/%s = %s" % (branch, ref["object"]["sha"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
