"""Bootstrap SSH key auth on the LubanCat board (root@192.168.1.111).

Password auth to append the local ed25519 public key to
/root/.ssh/authorized_keys, then verifies with a probe command.

The board's sshd intermittently resets connections pre-auth (likely
MaxStartups probabilistic drop), so connect() is retried.
"""
import sys
import time
import paramiko

HOST = "192.168.1.111"
USER = "root"
PASS = "Testpassword1234@"
PUBKEY_PATH = r"C:\Users\YAO\.ssh\id_ed25519.pub"
TRIES = 10


def connect():
    last = None
    for i in range(TRIES):
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            client.connect(
                HOST,
                username=USER,
                password=PASS,
                look_for_keys=False,
                allow_agent=False,
                timeout=10,
                banner_timeout=15,
            )
            print("[+] password auth OK (attempt %d)" % (i + 1))
            return client
        except (ConnectionResetError, paramiko.SSHException, OSError) as e:
            print("[-] attempt %d failed: %s" % (i + 1, e))
            client.close()
            time.sleep(2)
    raise SystemExit("could not connect after %d attempts" % TRIES)


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
    cmd = (
        'mkdir -p ~/.ssh && chmod 700 ~/.ssh && '
        'touch ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && '
        '(grep -qF "{key}" ~/.ssh/authorized_keys || echo "{key}" >> ~/.ssh/authorized_keys)'
    ).format(key=pubkey)
    rc, out, err = run(client, cmd)
    print("[+] key install rc=%d %s" % (rc, err.strip()))

    rc, out, err = run(client, "hostname; uname -a; id")
    print("[probe]\n%s%s" % (out, err))
    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
