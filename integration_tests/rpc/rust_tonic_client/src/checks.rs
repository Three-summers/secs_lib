use anyhow::{Result, anyhow, ensure};

use crate::rpc::RpcStatus;

pub fn expect_ok(status: Option<&RpcStatus>, what: &str) -> Result<()> {
    let status = status.ok_or_else(|| anyhow!("{what}: missing RpcStatus"))?;
    ensure!(status.ok == Some(true), "{what}: non-OK status: {status:?}");
    ensure!(
        status.error.is_none(),
        "{what}: OK status unexpectedly contains RpcError: {status:?}"
    );
    Ok(())
}

pub fn expect_error(status: Option<&RpcStatus>, what: &str, expected_message: &str) -> Result<()> {
    let status = status.ok_or_else(|| anyhow!("{what}: missing RpcStatus"))?;
    ensure!(
        status.ok == Some(false),
        "{what}: unexpectedly succeeded: {status:?}"
    );
    let error = status
        .error
        .as_ref()
        .ok_or_else(|| anyhow!("{what}: missing RpcError"))?;
    let message = error.message.as_deref().unwrap_or_default();
    ensure!(
        message.contains(expected_message),
        "{what}: expected error containing {expected_message:?}, got {error:?}"
    );
    ensure!(
        error
            .category
            .as_deref()
            .is_some_and(|value| !value.is_empty()),
        "{what}: missing error category: {error:?}"
    );
    ensure!(
        error.value.is_some(),
        "{what}: missing error value: {error:?}"
    );
    Ok(())
}
