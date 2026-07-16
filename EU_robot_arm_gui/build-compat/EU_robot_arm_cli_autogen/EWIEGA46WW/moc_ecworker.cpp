/****************************************************************************
** Meta object code from reading C++ file 'ecworker.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../ecworker.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ecworker.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_EcWorker_t {
    QByteArrayData data[15];
    char stringdata0[161];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_EcWorker_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_EcWorker_t qt_meta_stringdata_EcWorker = {
    {
QT_MOC_LITERAL(0, 0, 8), // "EcWorker"
QT_MOC_LITERAL(1, 9, 10), // "logMessage"
QT_MOC_LITERAL(2, 20, 0), // ""
QT_MOC_LITERAL(3, 21, 3), // "msg"
QT_MOC_LITERAL(4, 25, 13), // "masterStarted"
QT_MOC_LITERAL(5, 39, 13), // "masterStopped"
QT_MOC_LITERAL(6, 53, 13), // "errorOccurred"
QT_MOC_LITERAL(7, 67, 11), // "sdoFinished"
QT_MOC_LITERAL(8, 79, 9), // "SdoResult"
QT_MOC_LITERAL(9, 89, 3), // "res"
QT_MOC_LITERAL(10, 93, 20), // "motionRecordFinished"
QT_MOC_LITERAL(11, 114, 14), // "RecordedMotion"
QT_MOC_LITERAL(12, 129, 6), // "motion"
QT_MOC_LITERAL(13, 136, 18), // "motionStateChanged"
QT_MOC_LITERAL(14, 155, 5) // "state"

    },
    "EcWorker\0logMessage\0\0msg\0masterStarted\0"
    "masterStopped\0errorOccurred\0sdoFinished\0"
    "SdoResult\0res\0motionRecordFinished\0"
    "RecordedMotion\0motion\0motionStateChanged\0"
    "state"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_EcWorker[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       7,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       7,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   49,    2, 0x06 /* Public */,
       4,    0,   52,    2, 0x06 /* Public */,
       5,    0,   53,    2, 0x06 /* Public */,
       6,    1,   54,    2, 0x06 /* Public */,
       7,    1,   57,    2, 0x06 /* Public */,
      10,    1,   60,    2, 0x06 /* Public */,
      13,    1,   63,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, 0x80000000 | 8,    9,
    QMetaType::Void, 0x80000000 | 11,   12,
    QMetaType::Void, QMetaType::QString,   14,

       0        // eod
};

void EcWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<EcWorker *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->logMessage((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 1: _t->masterStarted(); break;
        case 2: _t->masterStopped(); break;
        case 3: _t->errorOccurred((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 4: _t->sdoFinished((*reinterpret_cast< SdoResult(*)>(_a[1]))); break;
        case 5: _t->motionRecordFinished((*reinterpret_cast< RecordedMotion(*)>(_a[1]))); break;
        case 6: _t->motionStateChanged((*reinterpret_cast< QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 5:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< RecordedMotion >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (EcWorker::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&EcWorker::logMessage)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (EcWorker::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&EcWorker::masterStarted)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (EcWorker::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&EcWorker::masterStopped)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (EcWorker::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&EcWorker::errorOccurred)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (EcWorker::*)(SdoResult );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&EcWorker::sdoFinished)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (EcWorker::*)(RecordedMotion );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&EcWorker::motionRecordFinished)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (EcWorker::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&EcWorker::motionStateChanged)) {
                *result = 6;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject EcWorker::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_meta_stringdata_EcWorker.data,
    qt_meta_data_EcWorker,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *EcWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *EcWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_EcWorker.stringdata0))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int EcWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QThread::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    }
    return _id;
}

// SIGNAL 0
void EcWorker::logMessage(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void EcWorker::masterStarted()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void EcWorker::masterStopped()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void EcWorker::errorOccurred(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void EcWorker::sdoFinished(SdoResult _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void EcWorker::motionRecordFinished(RecordedMotion _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void EcWorker::motionStateChanged(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
