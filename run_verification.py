
import subprocess
import time
import sys
import os

def main():
    env = os.environ.copy()
    lib_path = os.path.abspath("vendor/dist/lib")
    env["LD_LIBRARY_PATH"] = lib_path + ":" + env.get("LD_LIBRARY_PATH", "")
    print(f"Set LD_LIBRARY_PATH={lib_path}")

    print("Starting Server...")
    env["PYTHONUNBUFFERED"] = "1"
    server_proc = subprocess.Popen([sys.executable, "-u", "test_asgi.py"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    
    # Wait for server to start
    time.sleep(2)
    
    print("Starting Client...")
    
    # Also pass env to client (though it doesn't use msquic, doesn't hurt)
    client_res = subprocess.run([sys.executable, "test_http3_client.py"], capture_output=True, text=True, env=env)
    
    print("Client Output:")
    print(client_res.stdout)
    if client_res.stderr:
        print("Client Error:")
        print(client_res.stderr)
        
    print("Terminating Server...")
    server_proc.terminate()
    try:
        out, err = server_proc.communicate(timeout=2)
        print("Server Output:")
        print(out)
        if err:
            print("Server Error:")
            print(err)
    except subprocess.TimeoutError:
        server_proc.kill()
        
    # Check success
    # Check success
    # Output might be split across multiple lines in the logs, so check for presence of all parts
    output = client_res.stdout
    if "Part 1: Hello" in output and "Part 2: Stream!" in output:
        print("\nSUCCESS: Received Streaming Echo Response!")
        sys.exit(0)
    else:
        print("\nFAILURE: Did not receive Streaming Echo Response.")
        print("Output was:", output)
        sys.exit(1)

if __name__ == "__main__":
    main()
