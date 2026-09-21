"""SSH bootstrap using keyboard-interactive auth (server may refuse the
'password' method but allow keyboard-interactive), then installs the local
ed25519 public key for key-based auth.
"""
import sys
import time
import paramiko

HOST = "192.168.1.111"
USER = "root"
PASS = "root"
PUBKEY_PATH = r"C:\Users\YAO\.ssh\id_ed25519.pub"
TRIES = 6


def connect():
    for i in range(TRIES):
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            client.connect(HOST, username=USER, password=PASS,
                           look_for_keys=False, allow_agent=False, timeout=10)
            print("[+] 'password' auth OK (attempt %d)" % (i + 1))
            return client
        except paramiko.AuthenticationException:
            print("[-] password method rejected (attempt %d), trying keyboard-interactive" % (i + 1))
            try:
                t = client.get_transport()
                if t is None or not t.is_active():
                    continue

                def handler(title, instructions, prompts):
                    print("    [kbd] prompts: %s" % [p[0] for p in prompts])
                    return [PASS] * len(prompts)

                t.auth_interactive(USER, handler)
                print("[+] keyboard-interactive auth OK (attempt %d)" % (i + 1))
                return client
            except Exception as e:
                print("[-] keyboard-interactive also failed: %s" % e)
        except Exception as e:
            print("[-] connect attempt %d: %s" % (i + 1, e))
        client.close()
        time.sleep(2)
    return None


def run(client, cmd, timeout=30):
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode(errors="replace")
    err = stderr.read().decode(errors="replace")
    rc = stdout.channel.recv_exit_status()
    return rc, out, err


def main():
    with open(PUBKEY_PATH) as f:
        pubkey = f.read().strip()

    client = connect()
    if client is None:
        return 1

    cmd = (
        'mkdir -p ~/.ssh && chmod 700 ~/.ssh && '
        'touch ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && '
        '(grep -qF "{key}" ~/.ssh/authorized_keys || echo "{key}" >> ~/.ssh/authorized_keys)'
    ).format(key=pubkey)
    rc, out, err = run(client, cmd)
    print("[+] key install rc=%d %s" % (rc, err.strip()))

    rc, out, err = run(client, "hostname; uname -a; grep -E 'PermitRootLogin|PasswordAuthentication' /etc/ssh/sshd_config")
    print("[probe]\n%s%s" % (out, err))
    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
