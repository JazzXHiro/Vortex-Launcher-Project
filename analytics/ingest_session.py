import sys
import uuid
from datetime import datetime
from db import get_connection


def get_user_id(username):
    conn = get_connection()
    cur = conn.cursor()

    cur.execute(
        "SELECT user_id FROM users WHERE username=?",
        (username,)
    )

    result = cur.fetchone()

    cur.close()
    conn.close()

    if result:
        return result[0]

    return None


def get_game_id(game_name):
    conn = get_connection()
    cur = conn.cursor()

    cur.execute(
        "SELECT game_id FROM games WHERE LOWER(name)=LOWER(?)",
        (game_name,)
    )

    result = cur.fetchone()

    cur.close()
    conn.close()

    if result:
        return result[0]

    return None


def save_session(
    username,
    game_name,
    start_epoch,
    end_epoch,
    active_seconds,
    idle_seconds
):
    user_id = get_user_id(username)

    if not user_id:
        print("User not found")
        return

    game_id = get_game_id(game_name)

    if not game_id:
        print("Game not found in DB")
        return

    start_time = datetime.fromtimestamp(
        int(start_epoch)
    )

    end_time = datetime.fromtimestamp(
        int(end_epoch)
    )

    duration = int(end_epoch) - int(start_epoch)

    activity_ratio = (
        int(active_seconds) / duration
    )

    conn = get_connection()
    cur = conn.cursor()

    cur.execute("""
        INSERT INTO sessions (
            session_id,
            user_id,
            game_id,
            started_at,
            ended_at,
            duration_seconds,
            active_seconds,
            idle_seconds,
            activity_ratio
        )
        VALUES (?,?,?,?,?,?,?,?,?)
    """, (
        str(uuid.uuid4()),
        user_id,
        game_id,
        start_time,
        end_time,
        duration,
        active_seconds,
        idle_seconds,
        activity_ratio
    ))

    conn.commit()

    cur.close()
    conn.close()

    print("Session inserted")


if __name__ == "__main__":
    username = sys.argv[1]
    game_name = sys.argv[2]
    start_epoch = sys.argv[3]
    end_epoch = sys.argv[4]
    active_seconds = sys.argv[5]
    idle_seconds = sys.argv[6]

    save_session(
        username,
        game_name,
        start_epoch,
        end_epoch,
        active_seconds,
        idle_seconds
    )