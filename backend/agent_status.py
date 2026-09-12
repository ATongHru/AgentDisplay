"""AgentLamp-compatible status vocabulary for the local adapter."""

AGENTLAMP_STATUSES = {
    "IDLE",
    "THINKING",
    "CODING",
    "READING",
    "TESTING",
    "WAITING",
    "DONE",
    "ERROR",
    "OFFLINE",
    "STALE",
    "UNKNOWN",
}

STATUS_DETAILS = {"compacting", "tool_running", "subagent", "unknown"}
TOOL_CATEGORIES = {"read", "edit", "test", "shell", "mcp", "approval", "error"}
TASK_LABELS = {
    "implementing",
    "debugging",
    "testing",
    "reviewing",
    "refactoring",
    "reading",
    "planning",
    "waiting",
    "idle",
    "unknown",
}

# Existing project clients used this name before AgentLamp compatibility was added.
LEGACY_STATUS = {"tool_call": ("THINKING", "tool_running")}


def normalize_status(value: str) -> tuple[str, str | None]:
    value = value.strip()
    legacy = LEGACY_STATUS.get(value.lower())
    if legacy:
        return legacy
    status = value.upper()
    if status not in AGENTLAMP_STATUSES:
        raise ValueError("unsupported status")
    return status, None
