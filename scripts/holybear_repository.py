"""Exact GitHub Actions identity check for the HolyBear repository."""

from collections.abc import Mapping

REPOSITORY_ID = "1376078644"
OWNER_ID = "50009618"
OWNER = "HolyBearTW"
REPOSITORIES = frozenset(("HolyBearTW/nitlink", "HolyBearTW/nitlink-holybear-edition"))


def is_authorized_actions_repository(environment: Mapping[str, str]) -> bool:
    """Require immutable IDs, exact owner identity, and an allowed current slug."""
    return (
        environment.get("GITHUB_ACTIONS") == "true"
        and environment.get("GITHUB_REPOSITORY_ID") == REPOSITORY_ID
        and environment.get("GITHUB_REPOSITORY_OWNER_ID") == OWNER_ID
        and environment.get("GITHUB_REPOSITORY_OWNER") == OWNER
        and environment.get("GITHUB_REPOSITORY") in REPOSITORIES
    )
