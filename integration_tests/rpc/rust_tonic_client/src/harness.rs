use std::{
    process::Stdio,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, anyhow, bail};
use tokio::{net::TcpListener, process::Child, time};
use tonic::transport::{Channel, Endpoint};

use crate::Args;

pub struct TestProcesses {
    server: Option<Child>,
    peer: Option<Child>,
    endpoint: String,
    peer_port: Option<u16>,
    logs: String,
}

impl TestProcesses {
    pub async fn server_only(args: &Args) -> Result<Self> {
        Self::start(args, None).await
    }

    pub async fn with_peer(args: &Args, peer_args: &[&str]) -> Result<Self> {
        Self::start(args, Some(peer_args)).await
    }

    async fn start(args: &Args, peer_args: Option<&[&str]>) -> Result<Self> {
        let rpc_port = pick_port().await?;
        let peer_port = match peer_args {
            Some(_) => Some(pick_port().await?),
            None => None,
        };

        let peer = if let (Some(extra_args), Some(port)) = (peer_args, peer_port) {
            let mut command = tokio::process::Command::new(&args.peer);
            command
                .arg("--listen")
                .arg(format!("127.0.0.1:{port}"))
                .arg("--session-id")
                .arg("1")
                .args(extra_args)
                .stdout(Stdio::piped())
                .stderr(Stdio::piped());
            Some(command.spawn().context("spawn HSMS peer")?)
        } else {
            None
        };

        let mut server_command = tokio::process::Command::new(&args.server);
        server_command
            .arg("--listen")
            .arg(format!("127.0.0.1:{rpc_port}"))
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let server = server_command.spawn().context("spawn RPC server")?;

        let group = Self {
            server: Some(server),
            peer,
            endpoint: format!("http://127.0.0.1:{rpc_port}"),
            peer_port,
            logs: String::new(),
        };

        if let Err(error) = group.connect().await {
            return group
                .finish(Err(error.context("RPC server did not become ready")))
                .await
                .and_then(|_| Err(anyhow!("unreachable")));
        }
        Ok(group)
    }

    pub fn peer_port(&self) -> Result<u16> {
        self.peer_port.context("test group has no HSMS peer")
    }

    pub async fn kill_peer(&mut self) -> Result<()> {
        let child = self.peer.take().context("test group has no HSMS peer")?;
        self.logs
            .push_str(&collect_process("hsms-peer", child).await);
        Ok(())
    }

    pub async fn kill_server(&mut self) -> Result<()> {
        let child = self.server.take().context("test group has no RPC server")?;
        self.logs
            .push_str(&collect_process("rpc-server", child).await);
        Ok(())
    }

    pub async fn connect(&self) -> Result<Channel> {
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            let endpoint = Endpoint::from_shared(self.endpoint.clone())?
                .connect_timeout(Duration::from_millis(300))
                .timeout(Duration::from_secs(5));
            match endpoint.connect().await {
                Ok(channel) => return Ok(channel),
                Err(error) if Instant::now() < deadline => {
                    if self.server_exited()? {
                        bail!("RPC server exited before accepting connections");
                    }
                    time::sleep(Duration::from_millis(100)).await;
                    let _ = error;
                }
                Err(error) => return Err(error.into()),
            }
        }
    }

    fn server_exited(&self) -> Result<bool> {
        let Some(server) = self.server.as_ref() else {
            return Ok(true);
        };
        match server.id() {
            Some(_) => Ok(false),
            None => Ok(true),
        }
    }

    pub async fn finish(mut self, result: Result<()>) -> Result<()> {
        let logs = self.stop_and_collect().await;
        match result {
            Ok(()) => Ok(()),
            Err(error) => Err(error.context(logs)),
        }
    }

    async fn stop_and_collect(&mut self) -> String {
        let mut logs = std::mem::take(&mut self.logs);
        if let Some(child) = self.server.take() {
            logs.push_str(&collect_process("rpc-server", child).await);
        }
        if let Some(child) = self.peer.take() {
            logs.push_str(&collect_process("hsms-peer", child).await);
        }
        logs
    }
}

impl Drop for TestProcesses {
    fn drop(&mut self) {
        if let Some(server) = self.server.as_mut() {
            let _ = server.start_kill();
        }
        if let Some(peer) = self.peer.as_mut() {
            let _ = peer.start_kill();
        }
    }
}

async fn pick_port() -> Result<u16> {
    let listener = TcpListener::bind(("127.0.0.1", 0))
        .await
        .context("bind an ephemeral localhost port")?;
    Ok(listener.local_addr()?.port())
}

async fn collect_process(name: &str, mut child: Child) -> String {
    let _ = child.start_kill();
    match time::timeout(Duration::from_secs(5), child.wait_with_output()).await {
        Ok(Ok(output)) => format!(
            "\n[{name} stdout]\n{}\n[{name} stderr]\n{}\n",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ),
        Ok(Err(error)) => format!("\n[{name}] failed to collect output: {error}\n"),
        Err(_) => format!("\n[{name}] did not exit within 5 seconds\n"),
    }
}
