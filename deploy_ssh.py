#!/usr/bin/env python3
"""SSH password login using PTY with prompt detection"""
import pty, os, select, time, subprocess, sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.8.4"
PASS = "root"
PUBKEY = open(os.path.expanduser("~/.ssh/id_rsa.pub")).read().strip()

def ssh_password(cmd, send_first=None):
    """Run SSH command, detect password prompt, send password"""
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp("ssh", ["ssh", "-oStrictHostKeyChecking=accept-new",
                          "-oNumberOfPasswordPrompts=1",
                          f"root@{HOST}"] + (["-tt"] if send_first else []) +
                   (shlex.split(cmd) if " " not in cmd.strip()[:3] else ["sh", "-c", cmd]))
    else:
        output = b""
        authenticated = False
        done = False

        while not done:
            r, w, e = select.select([fd], [], [], 0.5)
            if r:
                try:
                    chunk = os.read(fd, 4096)
                    if not chunk: break
                    output += chunk
                except: break
            else:
                # Timeout with no data - check if we need to send password
                if not authenticated:
                    if b"password" in output.lower():
                        os.write(fd, PASS.encode() + b"\n")
                        authenticated = True
                        time.sleep(0.5)
                        if send_first:
                            os.write(fd, send_first.encode() + b"\n")
                            time.sleep(0.3)
                            os.write(fd, b"exit\n")
                    elif time.time() - start_time > 10:
                        break
                else:
                    if time.time() - start_time > 15:
                        break

        os.close(fd)
        os.waitpid(pid, 0)
        return output.decode(errors="replace")

import shlex

start_time = time.time()

# Step 1: Copy SSH public key to board
print("=== Installing SSH key on board ===")
cmd = f'mkdir -p ~/.ssh && echo "{PUBKEY}" >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys'
pid, fd = pty.fork()
if pid == 0:
    os.execvp("ssh", ["ssh", "-oStrictHostKeyChecking=accept-new",
                      "-oNumberOfPasswordPrompts=1",
                      f"root@{HOST}", cmd])
else:
    output = b""
    authenticated = False
    while True:
        r, w, e = select.select([fd], [], [], 0.5)
        if r:
            try:
                chunk = os.read(fd, 4096)
                if not chunk: break
                output += chunk
            except: break
        else:
            if not authenticated:
                if b"password" in output.lower():
                    os.write(fd, PASS.encode() + b"\n")
                    authenticated = True
                    time.sleep(2)
            else:
                time.sleep(1)
                break
    os.close(fd)
    os.waitpid(pid, 0)
    print(output.decode(errors="replace"))

# Now try passwordless SSH
def ssh_nopw(cmd):
    r = subprocess.run(["ssh", "-oStrictHostKeyChecking=accept-new",
                        f"root@{HOST}", cmd],
                       capture_output=True, text=True, timeout=10)
    return r.stdout, r.stderr, r.returncode

def scp_nopw(src, dst):
    r = subprocess.run(["scp", "-oStrictHostKeyChecking=accept-new",
                        src, f"root@{HOST}:{dst}"],
                       capture_output=True, text=True, timeout=30)
    return r.returncode == 0

import glob
PKG = "/home/yao/embeded_web/package"

# Test passwordless SSH
print("=== Testing passwordless SSH ===")
out, err, rc = ssh_nopw("echo SSH_OK")
print(f"  rc={rc}, out={out.strip()}")

if rc != 0:
    print("SSH key installation may have failed, trying alternative...")
    sys.exit(1)

print("=== Creating directories ===")
print(ssh_nopw("mkdir -p /home/www/cgi-bin /var/log/boa /etc /tmp/boa_sessions"))

print("=== Copying boa binary ===")
scp_nopw(f"{PKG}/bin/boa", "/usr/sbin/")

print("=== Copying web files ===")
for f in glob.glob(f"{PKG}/www/*"):
    print(f"  {os.path.basename(f)}")
    scp_nopw(f, "/home/www/")

print("=== Copying CGI binaries ===")
for f in glob.glob(f"{PKG}/cgi-bin/*"):
    print(f"  {os.path.basename(f)}")
    scp_nopw(f, "/home/www/cgi-bin/")

print("=== Copying config ===")
scp_nopw(f"{PKG}/etc/boa.conf", "/etc/")
scp_nopw(f"{PKG}/etc/mime.types", "/etc/")

print("=== Setting permissions ===")
ssh_nopw("chmod 755 /usr/sbin/boa /home/www/cgi-bin/* && chmod 644 /home/www/* /etc/boa.conf /etc/mime.types")

print("=== Verification ===")
out, err, rc = ssh_nopw("ls -la /usr/sbin/boa /home/www/ /home/www/cgi-bin/ /etc/boa.conf 2>&1")
print(out)

print("\n=== DEPLOYMENT COMPLETE ===")
print(f"Start on board: boa -c /etc/")
print(f"Access web: http://{HOST}/")
