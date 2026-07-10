use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, ensure};
use tokio::{task::JoinSet, time};
use tonic::{
    Code, Request,
    transport::{Channel, Endpoint},
};

use crate::{
    Args,
    checks::{expect_error, expect_ok},
    harness::TestProcesses,
    items,
    rpc::{
        CreateSessionRequest, DeleteSessionRequest, GetLibraryInfoRequest, GetSessionRequest,
        HsmsConfig, ListSessionsRequest, MessageEnvelope, RequestRequest, SendRequest,
        SessionRuntimeConfig, SessionState, StartSessionRequest, StopSessionRequest,
        TransportConfig, TransportKind, library_service_client::LibraryServiceClient,
        messaging_service_client::MessagingServiceClient,
        session_service_client::SessionServiceClient,
    },
};

pub async fn run_contract_and_validation(args: &Args) -> Result<()> {
    let group = TestProcesses::server_only(args).await?;
    let result = async {
        let channel = group.connect().await?;
        test_library_contract(channel.clone()).await?;
        test_validation_errors(channel).await
    }
    .await;
    group.finish(result).await
}

pub async fn run_lifecycle_and_messaging(args: &Args) -> Result<()> {
    let group = TestProcesses::with_peer(args, &[]).await?;
    let result = async {
        let channel = group.connect().await?;
        let peer_port = group.peer_port()?;
        test_session_lifecycle_and_messaging(channel, peer_port).await
    }
    .await;
    group.finish(result).await
}

pub async fn run_fault_and_concurrency_tests(args: &Args) -> Result<()> {
    test_application_timeout(args).await?;
    test_tonic_deadline(args).await?;
    test_unavailable_server().await?;
    test_peer_disconnect(args).await?;
    test_server_disconnect(args).await?;
    test_parallel_reads(args).await?;
    test_parallel_requests(args).await?;
    test_pending_limit(args).await?;
    test_stop_with_pending_request(args).await
}

async fn test_library_contract(channel: Channel) -> Result<()> {
    let mut client = LibraryServiceClient::new(channel);
    let response = client
        .get_library_info(GetLibraryInfoRequest {})
        .await?
        .into_inner();
    expect_ok(response.status.as_ref(), "GetLibraryInfo")?;
    ensure!(
        response
            .version
            .as_deref()
            .is_some_and(|value| !value.is_empty()),
        "GetLibraryInfo returned an empty version"
    );
    for transport in ["HSMS", "SECS-I"] {
        ensure!(
            response
                .supported_transports
                .iter()
                .any(|value| value == transport),
            "GetLibraryInfo missing transport {transport}: {response:?}"
        );
    }
    for feature in [
        "session-service-v1",
        "messaging-service-v1",
        "grpc-compatible-protocol",
        "itemnode",
    ] {
        ensure!(
            response
                .supported_features
                .iter()
                .any(|value| value == feature),
            "GetLibraryInfo missing feature {feature}: {response:?}"
        );
    }
    Ok(())
}

async fn test_validation_errors(channel: Channel) -> Result<()> {
    let mut sessions = SessionServiceClient::new(channel.clone());
    let mut messaging = MessagingServiceClient::new(channel);

    let response = sessions
        .create_session(CreateSessionRequest {
            name: Some("missing-transport".into()),
            transport: None,
            runtime: None,
        })
        .await?
        .into_inner();
    expect_error(
        response.status.as_ref(),
        "CreateSession(no transport)",
        "transport is required",
    )?;

    for (port, expected) in [
        (0, "hsms.port must be in range [1, 65535]"),
        (65_536, "hsms.port must be in range [1, 65535]"),
    ] {
        let response = sessions
            .create_session(create_hsms_request("invalid-port", port, 1, None))
            .await?
            .into_inner();
        expect_error(
            response.status.as_ref(),
            "CreateSession(invalid port)",
            expected,
        )?;
    }

    let response = sessions
        .create_session(create_hsms_request(
            "invalid-session-id",
            5000,
            0x8000,
            None,
        ))
        .await?
        .into_inner();
    expect_error(
        response.status.as_ref(),
        "CreateSession(invalid session id)",
        "hsms.session_id must be in range [0, 32767]",
    )?;

    for operation in ["get", "start", "stop", "delete"] {
        match operation {
            "get" => {
                let response = sessions
                    .get_session(GetSessionRequest {
                        session_id: Some("missing-session".into()),
                    })
                    .await?
                    .into_inner();
                expect_error(
                    response.status.as_ref(),
                    "GetSession(not found)",
                    "session not found",
                )?;
            }
            "start" => {
                let response = sessions
                    .start_session(StartSessionRequest {
                        session_id: Some("missing-session".into()),
                    })
                    .await?
                    .into_inner();
                expect_error(
                    response.status.as_ref(),
                    "StartSession(not found)",
                    "session not found",
                )?;
            }
            "stop" => {
                let response = sessions
                    .stop_session(StopSessionRequest {
                        session_id: Some("missing-session".into()),
                        reason: Some("validation".into()),
                    })
                    .await?
                    .into_inner();
                expect_error(
                    response.status.as_ref(),
                    "StopSession(not found)",
                    "session not found",
                )?;
            }
            "delete" => {
                let response = sessions
                    .delete_session(DeleteSessionRequest {
                        session_id: Some("missing-session".into()),
                    })
                    .await?
                    .into_inner();
                expect_error(
                    response.status.as_ref(),
                    "DeleteSession(not found)",
                    "session not found",
                )?;
            }
            _ => unreachable!(),
        }
    }

    let created = sessions
        .create_session(create_hsms_request("validation-session", 5000, 1, None))
        .await?
        .into_inner();
    expect_ok(created.status.as_ref(), "CreateSession(validation)")?;
    let session_id = created
        .session
        .and_then(|session| session.session_id)
        .context("CreateSession(validation) missing session id")?;

    let response = messaging
        .send(SendRequest {
            session_id: Some(session_id.clone()),
            message: None,
        })
        .await?
        .into_inner();
    expect_error(
        response.status.as_ref(),
        "Send(no message)",
        "message is required",
    )?;

    for (what, message, expected) in [
        (
            "Send(stream=128)",
            envelope(128, 3, Some(items::ascii("bad"))),
            "stream must be in range [0, 127]",
        ),
        (
            "Send(function=0)",
            envelope(1, 0, Some(items::ascii("bad"))),
            "function must be a non-zero primary function",
        ),
        (
            "Send(function=2)",
            envelope(1, 2, Some(items::ascii("bad"))),
            "function must be a non-zero primary function",
        ),
    ] {
        let response = messaging
            .send(SendRequest {
                session_id: Some(session_id.clone()),
                message: Some(message),
            })
            .await?
            .into_inner();
        expect_error(response.status.as_ref(), what, expected)?;
    }

    let response = messaging
        .request(RequestRequest {
            session_id: Some(session_id.clone()),
            request: None,
            timeout_ms: Some(100),
        })
        .await?
        .into_inner();
    expect_error(
        response.status.as_ref(),
        "Request(no message)",
        "request message is required",
    )?;

    let response = messaging
        .request(RequestRequest {
            session_id: Some(session_id.clone()),
            request: Some(envelope(1, 2, Some(items::ascii("bad")))),
            timeout_ms: Some(100),
        })
        .await?
        .into_inner();
    expect_error(
        response.status.as_ref(),
        "Request(function=2)",
        "function must be a non-zero primary function",
    )?;

    let response = sessions
        .delete_session(DeleteSessionRequest {
            session_id: Some(session_id),
        })
        .await?
        .into_inner();
    expect_ok(response.status.as_ref(), "DeleteSession(validation)")?;
    Ok(())
}

async fn test_session_lifecycle_and_messaging(channel: Channel, peer_port: u16) -> Result<()> {
    let mut sessions = SessionServiceClient::new(channel.clone());
    let mut messaging = MessagingServiceClient::new(channel);
    let runtime = SessionRuntimeConfig {
        request_timeout_ms: Some(1_500),
        poll_interval_ms: Some(10),
        max_pending_requests: Some(16),
        enable_dump: Some(false),
        dump_tx: Some(false),
        dump_rx: Some(false),
        enable_secs2_decode_in_dump: Some(false),
    };
    let created = sessions
        .create_session(create_hsms_request(
            "rust-tonic-hsms",
            peer_port as u32,
            1,
            Some(runtime),
        ))
        .await?
        .into_inner();
    expect_ok(created.status.as_ref(), "CreateSession(lifecycle)")?;
    let created_session = created
        .session
        .context("CreateSession(lifecycle) missing session")?;
    let session_id = created_session
        .session_id
        .clone()
        .context("CreateSession(lifecycle) missing session id")?;
    ensure!(
        created_session.name.as_deref() == Some("rust-tonic-hsms"),
        "CreateSession did not preserve name: {created_session:?}"
    );
    ensure!(
        created_session.state == Some(SessionState::Created as i32),
        "CreateSession returned wrong state: {created_session:?}"
    );
    ensure!(
        created_session.running == Some(false),
        "CreateSession returned running session: {created_session:?}"
    );
    ensure!(
        created_session
            .transport
            .as_ref()
            .and_then(|value| value.kind)
            == Some(TransportKind::Hsms as i32),
        "CreateSession did not echo HSMS transport: {created_session:?}"
    );
    ensure!(
        created_session
            .runtime
            .as_ref()
            .and_then(|value| value.max_pending_requests)
            == Some(16),
        "CreateSession did not echo runtime: {created_session:?}"
    );

    let fetched = sessions
        .get_session(GetSessionRequest {
            session_id: Some(session_id.clone()),
        })
        .await?
        .into_inner();
    expect_ok(fetched.status.as_ref(), "GetSession(created)")?;
    ensure!(
        fetched
            .session
            .as_ref()
            .and_then(|session| session.session_id.as_deref())
            == Some(session_id.as_str()),
        "GetSession returned wrong session: {fetched:?}"
    );

    let listed = sessions
        .list_sessions(ListSessionsRequest {})
        .await?
        .into_inner();
    expect_ok(listed.status.as_ref(), "ListSessions(created)")?;
    ensure!(
        listed.sessions.len() == 1
            && listed.sessions[0].session_id.as_deref() == Some(session_id.as_str()),
        "ListSessions returned unexpected sessions: {listed:?}"
    );

    let started = sessions
        .start_session(StartSessionRequest {
            session_id: Some(session_id.clone()),
        })
        .await?
        .into_inner();
    expect_ok(started.status.as_ref(), "StartSession")?;
    ensure!(
        started
            .session
            .as_ref()
            .and_then(|session| session.session_id.as_deref())
            == Some(session_id.as_str()),
        "StartSession returned wrong session: {started:?}"
    );
    wait_for_selected(&mut sessions, &session_id).await?;

    let started_again = sessions
        .start_session(StartSessionRequest {
            session_id: Some(session_id.clone()),
        })
        .await?
        .into_inner();
    expect_ok(started_again.status.as_ref(), "StartSession(idempotent)")?;

    let sent = messaging
        .send(SendRequest {
            session_id: Some(session_id.clone()),
            message: Some(envelope(1, 3, Some(items::ascii("ONEWAY")))),
        })
        .await?
        .into_inner();
    expect_ok(sent.status.as_ref(), "Send")?;
    let accepted = sent.accepted.context("Send missing accepted envelope")?;
    ensure!(
        accepted.stream == Some(1) && accepted.function == Some(3),
        "Send returned wrong accepted header: {accepted:?}"
    );
    ensure!(
        accepted.decoded_item == Some(items::ascii("ONEWAY")),
        "Send returned wrong accepted item: {accepted:?}"
    );

    request_and_assert(&mut messaging, &session_id, items::ping(7), |reply| {
        ensure!(
            reply == &items::list(vec![items::ascii("ACK"), items::ping(7)]),
            "unexpected ping reply: {reply:?}"
        );
        Ok(())
    })
    .await?;

    let all_types = items::all_types();
    request_and_assert(
        &mut messaging,
        &session_id,
        all_types.clone(),
        move |reply| {
            ensure!(
                reply == &items::list(vec![items::ascii("ACK"), all_types]),
                "all ItemNode types changed across RPC/SECS-II echo: {reply:?}"
            );
            Ok(())
        },
    )
    .await?;

    let stopped = sessions
        .stop_session(StopSessionRequest {
            session_id: Some(session_id.clone()),
            reason: Some("rust integration complete".into()),
        })
        .await?
        .into_inner();
    expect_ok(stopped.status.as_ref(), "StopSession")?;
    ensure!(
        stopped.session.as_ref().and_then(|session| session.state)
            == Some(SessionState::Stopped as i32),
        "StopSession returned wrong state: {stopped:?}"
    );

    let stopped_again = sessions
        .stop_session(StopSessionRequest {
            session_id: Some(session_id.clone()),
            reason: Some("idempotency check".into()),
        })
        .await?
        .into_inner();
    expect_ok(stopped_again.status.as_ref(), "StopSession(idempotent)")?;

    let deleted = sessions
        .delete_session(DeleteSessionRequest {
            session_id: Some(session_id.clone()),
        })
        .await?
        .into_inner();
    expect_ok(deleted.status.as_ref(), "DeleteSession")?;

    let missing = sessions
        .get_session(GetSessionRequest {
            session_id: Some(session_id),
        })
        .await?
        .into_inner();
    expect_error(
        missing.status.as_ref(),
        "GetSession(after delete)",
        "session not found",
    )?;
    Ok(())
}

async fn test_application_timeout(args: &Args) -> Result<()> {
    let group = TestProcesses::with_peer(args, &["--drop-s1f1"]).await?;
    let result = async {
        let channel = group.connect().await?;
        let peer_port = group.peer_port()?;
        let (mut sessions, mut messaging, session_id) =
            create_started_session(channel, peer_port, "application-timeout", 4, 500).await?;

        let response = time::timeout(
            Duration::from_secs(2),
            messaging.request(RequestRequest {
                session_id: Some(session_id.clone()),
                request: Some(MessageEnvelope {
                    w_bit: Some(true),
                    ..envelope(1, 1, Some(items::ping(101)))
                }),
                timeout_ms: Some(200),
            }),
        )
        .await
        .context("application timeout RPC did not finish")??
        .into_inner();
        expect_error(
            response.status.as_ref(),
            "Request(application timeout)",
            "timeout",
        )?;
        stop_and_delete(&mut sessions, &session_id).await
    }
    .await;
    group.finish(result).await
}

async fn test_tonic_deadline(args: &Args) -> Result<()> {
    let group = TestProcesses::with_peer(args, &["--reply-delay-ms", "1000"]).await?;
    let result = async {
        let channel = group.connect().await?;
        let peer_port = group.peer_port()?;
        let (_sessions, mut messaging, session_id) =
            create_started_session(channel, peer_port, "tonic-deadline", 4, 2_000).await?;

        let mut request = Request::new(RequestRequest {
            session_id: Some(session_id),
            request: Some(MessageEnvelope {
                w_bit: Some(true),
                ..envelope(1, 1, Some(items::ping(102)))
            }),
            timeout_ms: Some(1_500),
        });
        request.set_timeout(Duration::from_millis(100));
        let error = messaging
            .request(request)
            .await
            .expect_err("tonic deadline unexpectedly succeeded");
        ensure!(
            error.code() == Code::Cancelled && error.message() == "Timeout expired",
            "expected tonic client timeout cancellation, got {error:?}"
        );
        Ok(())
    }
    .await;
    group.finish(result).await
}

async fn test_unavailable_server() -> Result<()> {
    let listener = tokio::net::TcpListener::bind(("127.0.0.1", 0)).await?;
    let port = listener.local_addr()?.port();
    drop(listener);
    let result = Endpoint::from_shared(format!("http://127.0.0.1:{port}"))?
        .connect_timeout(Duration::from_millis(250))
        .connect()
        .await;
    ensure!(
        result.is_err(),
        "tonic connected successfully to an unused port"
    );
    Ok(())
}

async fn test_peer_disconnect(args: &Args) -> Result<()> {
    let mut group = TestProcesses::with_peer(args, &[]).await?;
    let result = async {
        let channel = group.connect().await?;
        let peer_port = group.peer_port()?;
        let (mut sessions, mut messaging, session_id) =
            create_started_session(channel, peer_port, "peer-disconnect", 4, 500).await?;
        group.kill_peer().await?;

        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let response = sessions
                .get_session(GetSessionRequest {
                    session_id: Some(session_id.clone()),
                })
                .await?
                .into_inner();
            expect_ok(response.status.as_ref(), "GetSession(peer disconnected)")?;
            let session = response
                .session
                .context("GetSession(peer disconnected) missing session")?;
            if session.state == Some(SessionState::Stopped as i32) {
                let error = session
                    .last_error
                    .context("peer disconnect did not record last_error")?;
                ensure!(
                    error
                        .message
                        .as_deref()
                        .is_some_and(|message| !message.is_empty()),
                    "peer disconnect recorded an empty last_error: {error:?}"
                );
                break;
            }
            ensure!(
                Instant::now() < deadline,
                "session did not stop after peer disconnect: {session:?}"
            );
            time::sleep(Duration::from_millis(50)).await;
        }

        let response = messaging
            .send(SendRequest {
                session_id: Some(session_id.clone()),
                message: Some(envelope(1, 3, Some(items::ascii("AFTER-DISCONNECT")))),
            })
            .await?
            .into_inner();
        expect_error(
            response.status.as_ref(),
            "Send(after peer disconnect)",
            "session is not running",
        )?;
        let deleted = sessions
            .delete_session(DeleteSessionRequest {
                session_id: Some(session_id),
            })
            .await?
            .into_inner();
        expect_ok(
            deleted.status.as_ref(),
            "DeleteSession(after peer disconnect)",
        )
    }
    .await;
    group.finish(result).await
}

async fn test_server_disconnect(args: &Args) -> Result<()> {
    let mut group = TestProcesses::server_only(args).await?;
    let result = async {
        let channel = group.connect().await?;
        let mut client = LibraryServiceClient::new(channel);
        group.kill_server().await?;

        let mut request = Request::new(GetLibraryInfoRequest {});
        request.set_timeout(Duration::from_millis(500));
        ensure!(
            client.get_library_info(request).await.is_err(),
            "RPC unexpectedly succeeded after server exit"
        );
        Ok(())
    }
    .await;
    group.finish(result).await
}

async fn test_parallel_reads(args: &Args) -> Result<()> {
    let group = TestProcesses::server_only(args).await?;
    let result = async {
        let channel = group.connect().await?;
        let mut tasks = JoinSet::new();
        for index in 0..32 {
            let channel = channel.clone();
            tasks.spawn(async move {
                if index % 2 == 0 {
                    let mut client = LibraryServiceClient::new(channel);
                    let response = client
                        .get_library_info(GetLibraryInfoRequest {})
                        .await?
                        .into_inner();
                    expect_ok(response.status.as_ref(), "parallel GetLibraryInfo")
                } else {
                    let mut client = SessionServiceClient::new(channel);
                    let response = client
                        .list_sessions(ListSessionsRequest {})
                        .await?
                        .into_inner();
                    expect_ok(response.status.as_ref(), "parallel ListSessions")
                }
            });
        }
        while let Some(result) = tasks.join_next().await {
            result.context("parallel read task panicked")??;
        }
        Ok(())
    }
    .await;
    group.finish(result).await
}

async fn test_parallel_requests(args: &Args) -> Result<()> {
    let group = TestProcesses::with_peer(args, &[]).await?;
    let result = async {
        let channel = group.connect().await?;
        let peer_port = group.peer_port()?;
        let (mut sessions, messaging, session_id) =
            create_started_session(channel, peer_port, "parallel-requests", 16, 1_500).await?;
        let mut tasks = JoinSet::new();
        for sequence in 0..8_u32 {
            let mut client = messaging.clone();
            let session_id = session_id.clone();
            tasks.spawn(async move {
                request_and_assert(
                    &mut client,
                    &session_id,
                    items::ping(sequence),
                    move |reply| {
                        ensure!(
                            reply
                                == &items::list(vec![items::ascii("ACK"), items::ping(sequence),]),
                            "parallel reply mismatch for sequence {sequence}: {reply:?}"
                        );
                        Ok(())
                    },
                )
                .await
            });
        }
        while let Some(result) = tasks.join_next().await {
            result.context("parallel request task panicked")??;
        }
        stop_and_delete(&mut sessions, &session_id).await
    }
    .await;
    group.finish(result).await
}

async fn test_pending_limit(args: &Args) -> Result<()> {
    let group = TestProcesses::with_peer(args, &["--reply-delay-ms", "500"]).await?;
    let result = async {
        let channel = group.connect().await?;
        let peer_port = group.peer_port()?;
        let (mut sessions, messaging, session_id) =
            create_started_session(channel, peer_port, "pending-limit", 1, 2_000).await?;

        let mut first_client = messaging.clone();
        let first_session = session_id.clone();
        let first = tokio::spawn(async move {
            request_and_assert(
                &mut first_client,
                &first_session,
                items::ping(201),
                |reply| {
                    ensure!(
                        reply == &items::list(vec![items::ascii("ACK"), items::ping(201)]),
                        "first pending request returned wrong reply: {reply:?}"
                    );
                    Ok(())
                },
            )
            .await
        });
        time::sleep(Duration::from_millis(100)).await;

        let mut second_client = messaging;
        let response = second_client
            .request(RequestRequest {
                session_id: Some(session_id.clone()),
                request: Some(MessageEnvelope {
                    w_bit: Some(true),
                    ..envelope(1, 1, Some(items::ping(202)))
                }),
                timeout_ms: Some(1_500),
            })
            .await?
            .into_inner();
        expect_error(
            response.status.as_ref(),
            "Request(pending limit)",
            "buffer overflow",
        )?;
        time::timeout(Duration::from_secs(2), first)
            .await
            .context("first pending request did not finish")?
            .context("first pending request task panicked")??;
        stop_and_delete(&mut sessions, &session_id).await
    }
    .await;
    group.finish(result).await
}

async fn test_stop_with_pending_request(args: &Args) -> Result<()> {
    let group = TestProcesses::with_peer(args, &["--drop-s1f1"]).await?;
    let result = async {
        let channel = group.connect().await?;
        let peer_port = group.peer_port()?;
        let (mut sessions, messaging, session_id) =
            create_started_session(channel, peer_port, "stop-with-pending", 4, 500).await?;

        let mut request_client = messaging;
        let request_session = session_id.clone();
        let pending = tokio::spawn(async move {
            request_client
                .request(RequestRequest {
                    session_id: Some(request_session),
                    request: Some(MessageEnvelope {
                        w_bit: Some(true),
                        ..envelope(1, 1, Some(items::ping(301)))
                    }),
                    timeout_ms: Some(500),
                })
                .await
        });
        time::sleep(Duration::from_millis(100)).await;

        let stopped = time::timeout(
            Duration::from_secs(2),
            sessions.stop_session(StopSessionRequest {
                session_id: Some(session_id.clone()),
                reason: Some("stop while request pending".into()),
            }),
        )
        .await
        .context("StopSession blocked with a pending request")??
        .into_inner();
        expect_ok(stopped.status.as_ref(), "StopSession(with pending request)")?;

        let pending_response = time::timeout(Duration::from_secs(2), pending)
            .await
            .context("pending request did not converge after StopSession")?
            .context("pending request task panicked")??
            .into_inner();
        expect_error(
            pending_response.status.as_ref(),
            "Request(stopped while pending)",
            "timeout",
        )?;

        let deleted = sessions
            .delete_session(DeleteSessionRequest {
                session_id: Some(session_id),
            })
            .await?
            .into_inner();
        expect_ok(deleted.status.as_ref(), "DeleteSession(after pending stop)")
    }
    .await;
    group.finish(result).await
}

async fn wait_for_selected(
    sessions: &mut SessionServiceClient<Channel>,
    session_id: &str,
) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let response = sessions
            .get_session(GetSessionRequest {
                session_id: Some(session_id.to_owned()),
            })
            .await?
            .into_inner();
        expect_ok(response.status.as_ref(), "GetSession(wait selected)")?;
        let session = response
            .session
            .context("GetSession(wait selected) missing session")?;
        if session.selected_generation.unwrap_or_default() > 0 {
            ensure!(
                session.state == Some(SessionState::Running as i32)
                    && session.running == Some(true),
                "selected session is not running: {session:?}"
            );
            return Ok(());
        }
        if session.state == Some(SessionState::Stopped as i32) {
            return Err(anyhow!(
                "session stopped before selection, last_error={:?}",
                session.last_error
            ));
        }
        ensure!(
            Instant::now() < deadline,
            "timed out waiting for HSMS selection: {session:?}"
        );
        time::sleep(Duration::from_millis(100)).await;
    }
}

async fn create_started_session(
    channel: Channel,
    peer_port: u16,
    name: &str,
    max_pending_requests: u32,
    request_timeout_ms: u32,
) -> Result<(
    SessionServiceClient<Channel>,
    MessagingServiceClient<Channel>,
    String,
)> {
    let mut sessions = SessionServiceClient::new(channel.clone());
    let messaging = MessagingServiceClient::new(channel);
    let response = sessions
        .create_session(create_hsms_request(
            name,
            peer_port as u32,
            1,
            Some(SessionRuntimeConfig {
                request_timeout_ms: Some(request_timeout_ms),
                poll_interval_ms: Some(10),
                max_pending_requests: Some(max_pending_requests),
                enable_dump: Some(false),
                dump_tx: Some(false),
                dump_rx: Some(false),
                enable_secs2_decode_in_dump: Some(false),
            }),
        ))
        .await?
        .into_inner();
    expect_ok(response.status.as_ref(), "CreateSession(started helper)")?;
    let session_id = response
        .session
        .and_then(|session| session.session_id)
        .context("CreateSession(started helper) missing session id")?;
    let response = sessions
        .start_session(StartSessionRequest {
            session_id: Some(session_id.clone()),
        })
        .await?
        .into_inner();
    expect_ok(response.status.as_ref(), "StartSession(started helper)")?;
    wait_for_selected(&mut sessions, &session_id).await?;
    Ok((sessions, messaging, session_id))
}

async fn stop_and_delete(
    sessions: &mut SessionServiceClient<Channel>,
    session_id: &str,
) -> Result<()> {
    let stopped = sessions
        .stop_session(StopSessionRequest {
            session_id: Some(session_id.to_owned()),
            reason: Some("test cleanup".into()),
        })
        .await?
        .into_inner();
    expect_ok(stopped.status.as_ref(), "StopSession(test cleanup)")?;
    let deleted = sessions
        .delete_session(DeleteSessionRequest {
            session_id: Some(session_id.to_owned()),
        })
        .await?
        .into_inner();
    expect_ok(deleted.status.as_ref(), "DeleteSession(test cleanup)")
}

async fn request_and_assert(
    messaging: &mut MessagingServiceClient<Channel>,
    session_id: &str,
    request_item: crate::rpc::ItemNode,
    assert_item: impl FnOnce(&crate::rpc::ItemNode) -> Result<()>,
) -> Result<()> {
    let response = messaging
        .request(RequestRequest {
            session_id: Some(session_id.to_owned()),
            request: Some(MessageEnvelope {
                w_bit: Some(true),
                ..envelope(1, 1, Some(request_item))
            }),
            timeout_ms: Some(1_500),
        })
        .await?
        .into_inner();
    expect_ok(response.status.as_ref(), "Request")?;
    let reply = response.reply.context("Request missing reply envelope")?;
    ensure!(
        reply.stream == Some(1) && reply.function == Some(2),
        "Request returned wrong reply header: {reply:?}"
    );
    ensure!(
        reply.body.as_ref().is_some_and(|body| !body.is_empty()),
        "Request returned an empty reply body: {reply:?}"
    );
    let decoded = reply
        .decoded_item
        .as_ref()
        .context("Request missing decoded_item")?;
    assert_item(decoded)
}

fn create_hsms_request(
    name: &str,
    port: u32,
    session_id: u32,
    runtime: Option<SessionRuntimeConfig>,
) -> CreateSessionRequest {
    CreateSessionRequest {
        name: Some(name.to_owned()),
        transport: Some(TransportConfig {
            kind: Some(TransportKind::Hsms as i32),
            hsms: Some(HsmsConfig {
                ip: Some("127.0.0.1".into()),
                port: Some(port),
                session_id: Some(session_id),
                passive: Some(false),
                auto_reconnect: Some(false),
                t3_ms: Some(1_500),
                t5_ms: Some(200),
                t6_ms: Some(1_500),
                t7_ms: Some(1_500),
                t8_ms: Some(1_500),
            }),
            secs1: None,
        }),
        runtime,
    }
}

fn envelope(
    stream: u32,
    function: u32,
    decoded_item: Option<crate::rpc::ItemNode>,
) -> MessageEnvelope {
    MessageEnvelope {
        stream: Some(stream),
        function: Some(function),
        w_bit: Some(false),
        system_bytes: None,
        body: None,
        decoded_item,
    }
}
