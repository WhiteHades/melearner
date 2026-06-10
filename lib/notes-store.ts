import type { Note } from "@/types"
import { isTauri } from "./tauri"
import {
  listNotesByLesson,
  saveNote as dbSaveNote,
  deleteNote as dbDeleteNote,
} from "./database"

export interface NoteStore {
  list(lessonId: string): Promise<Note[]>
  save(note: Note): Promise<void>
  remove(noteId: string): Promise<void>
}

class SqliteNoteStore implements NoteStore {
  async list(lessonId: string) {
    return listNotesByLesson(lessonId)
  }

  async save(note: Note) {
    await dbSaveNote(note)
  }

  async remove(noteId: string) {
    await dbDeleteNote(noteId)
  }
}

class MemoryNoteStore implements NoteStore {
  #notes: Note[] = []

  async list(lessonId: string) {
    return this.#notes.filter((n) => n.lessonId === lessonId)
  }

  async save(note: Note) {
    this.#notes.push(note)
  }

  async remove(noteId: string) {
    this.#notes = this.#notes.filter((n) => n.id !== noteId)
  }
}

let _store: NoteStore | null = null

export function getNoteStore(): NoteStore {
  if (!_store) {
    _store = isTauri() ? new SqliteNoteStore() : new MemoryNoteStore()
  }
  return _store
}
