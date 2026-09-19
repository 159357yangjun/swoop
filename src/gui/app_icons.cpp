// 应用自绘图标集：单色线性 + 语义着色，主题感知
#include "app_icons.h"
#include "category_filter_proxy.h"   // detectFileType：全项目唯一的文件后缀表

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPixmap>
#include <QIconEngine>
#include <QApplication>
#include <QGuiApplication>
#include <QHash>

namespace AppIcons {

// ── 主题配色 ────────────────────────────────
static bool g_dark = false;

void setDarkTheme(bool dark) { g_dark = dark; }
bool isDarkTheme() { return g_dark; }

static QColor fgColor()
{
    return g_dark ? QColor(0xc9, 0xcf, 0xd8) : QColor(0x3f, 0x46, 0x53);
}
static QColor disabledColor()
{
    return g_dark ? QColor(0x5c, 0x62, 0x6c) : QColor(0xbd, 0xc2, 0xcb);
}
static QColor activeColor()
{
    return g_dark ? QColor(0x4c, 0x8d, 0xff) : QColor(0x25, 0x63, 0xeb);
}

static QColor colorForMode(QIcon::Mode mode)
{
    switch (mode) {
        case QIcon::Disabled: return disabledColor();
        case QIcon::Active:
        case QIcon::Selected: return activeColor();
        default:              return fgColor();
    }
}

// ── 语义着色槽位 ────────────────────────────────
// 中低饱和、明暗两套各自保证对比度。刻意避开「每个类型一个高饱和彩虹色」：
// 这些色只用在文件类型图标上，其余界面仍然是中性灰 + 单一强调色。
static QColor tintColor(Tint t)
{
    struct Pair { QColor light, dark; };
    static const QHash<int, Pair> kTable {
        { int(Tint::Accent), { QColor(0x25, 0x63, 0xeb), QColor(0x4c, 0x8d, 0xff) } },
        { int(Tint::Green),  { QColor(0x2f, 0x9e, 0x6f), QColor(0x3f, 0xb2, 0x7f) } },
        { int(Tint::Amber),  { QColor(0xb0, 0x77, 0x1a), QColor(0xd8, 0xa0, 0x45) } },
        { int(Tint::Indigo), { QColor(0x4f, 0x46, 0xe5), QColor(0x87, 0x8d, 0xf8) } },
        { int(Tint::Purple), { QColor(0x7c, 0x4d, 0xff), QColor(0xa4, 0x8b, 0xff) } },
        { int(Tint::Rose),   { QColor(0xd0, 0x43, 0x54), QColor(0xf0, 0x6f, 0x7d) } },
        { int(Tint::Red),    { QColor(0xd0, 0x52, 0x52), QColor(0xe0, 0x65, 0x65) } },
        { int(Tint::Slate),  { QColor(0x6b, 0x72, 0x80), QColor(0x9a, 0xa0, 0xaa) } },
    };
    const Pair p = kTable.value(int(t));
    return g_dark ? p.dark : p.light;
}

// ── 字形绘制 ────────────────────────────────
// 统一 16×16 网格；描边线宽 1.5，圆头圆角。
void paintGlyph(QPainter* p, Glyph g, const QColor& c)
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);

    switch (g) {
    case Glyph::NewTask:
        // 向下箭头 + 底部托盘（下载语义；本行工具栏的主操作）
        p->drawLine(QPointF(8.0, 2.6), QPointF(8.0, 9.4));
        p->drawPolyline(QPolygonF() << QPointF(4.7, 6.3)
                                    << QPointF(8.0, 9.7)
                                    << QPointF(11.3, 6.3));
        p->drawPolyline(QPolygonF() << QPointF(3.0, 12.0)
                                    << QPointF(3.0, 13.4)
                                    << QPointF(13.0, 13.4)
                                    << QPointF(13.0, 12.0));
        break;

    case Glyph::Start: {
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawPolygon(QPolygonF() << QPointF(4.6, 3.0)
                                   << QPointF(12.6, 8.0)
                                   << QPointF(4.6, 13.0));
        break;
    }

    case Glyph::Restart: {
        // 环形箭头：从 60° 顺时针扫 290°（缺口留在顶部），箭头落在右上起点处
        const QRectF ring(3.0, 3.0, 10.0, 10.0);
        p->drawArc(ring, 60 * 16, -290 * 16);
        const QPointF tip(10.5, 3.67);          // 60° 处弧上点
        const QPointF dir(0.866, 0.5);          // 顺时针切线方向
        const QPointF nor(-0.5, 0.866);         // 切线法向
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawPolygon(QPolygonF() << (tip + dir * 2.3)
                                   << (tip + nor * 1.7)
                                   << (tip - nor * 1.7));
        break;
    }

    case Glyph::Pause:
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRoundedRect(QRectF(4.6, 3.2, 2.5, 9.6), 1.0, 1.0);
        p->drawRoundedRect(QRectF(8.9, 3.2, 2.5, 9.6), 1.0, 1.0);
        break;

    case Glyph::Cancel:
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRoundedRect(QRectF(4.0, 4.0, 8.0, 8.0), 1.8, 1.8);
        break;

    case Glyph::Remove:
        p->drawLine(QPointF(2.7, 4.7), QPointF(13.3, 4.7));            // 桶盖
        p->drawPolyline(QPolygonF() << QPointF(6.2, 4.7)               // 提手
                                    << QPointF(6.2, 2.9)
                                    << QPointF(9.8, 2.9)
                                    << QPointF(9.8, 4.7));
        p->drawPolyline(QPolygonF() << QPointF(4.4, 4.7)               // 桶身
                                    << QPointF(5.1, 13.3)
                                    << QPointF(10.9, 13.3)
                                    << QPointF(11.6, 4.7));
        p->drawLine(QPointF(6.9, 7.3), QPointF(7.1, 10.7));            // 两道竖纹
        p->drawLine(QPointF(9.1, 7.3), QPointF(8.9, 10.7));
        break;

    case Glyph::Queue:
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawEllipse(QPointF(3.6, 4.3), 0.95, 0.95);
        p->drawEllipse(QPointF(3.6, 8.0), 0.95, 0.95);
        p->drawEllipse(QPointF(3.6, 11.7), 0.95, 0.95);
        p->setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(QPointF(6.3, 4.3), QPointF(13.2, 4.3));
        p->drawLine(QPointF(6.3, 8.0), QPointF(13.2, 8.0));
        p->drawLine(QPointF(6.3, 11.7), QPointF(13.2, 11.7));
        break;

    case Glyph::Site:
        p->drawEllipse(QPointF(8.0, 8.0), 5.3, 5.3);                  // 球体
        p->drawLine(QPointF(2.7, 8.0), QPointF(13.3, 8.0));            // 赤道
        p->drawEllipse(QRectF(5.6, 2.9, 4.8, 10.2));                   // 经线
        break;

    case Glyph::History:
    case Glyph::Pending:
        p->drawEllipse(QPointF(8.0, 8.0), 5.3, 5.3);
        p->drawLine(QPointF(8.0, 8.0), QPointF(8.0, 4.6));             // 时针
        p->drawLine(QPointF(8.0, 8.0), QPointF(10.7, 9.5));            // 分针
        break;

    case Glyph::Settings:
        // 两条滑杆：线段中间留缺口放滑块，比齿轮更简洁、小尺寸下也认得出
        p->drawLine(QPointF(2.6, 4.9), QPointF(6.6, 4.9));
        p->drawLine(QPointF(10.8, 4.9), QPointF(13.4, 4.9));
        p->drawLine(QPointF(2.6, 11.1), QPointF(5.6, 11.1));
        p->drawLine(QPointF(9.8, 11.1), QPointF(13.4, 11.1));
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawEllipse(QPointF(8.7, 4.9), 1.85, 1.85);
        p->drawEllipse(QPointF(7.7, 11.1), 1.85, 1.85);
        break;

    case Glyph::Category:
        p->drawPolyline(QPolygonF() << QPointF(2.6, 12.6)              // 文件夹
                                    << QPointF(2.6, 4.0)
                                    << QPointF(6.3, 4.0)
                                    << QPointF(7.6, 5.6)
                                    << QPointF(13.4, 5.6)
                                    << QPointF(13.4, 12.6)
                                    << QPointF(2.6, 12.6));
        break;

    case Glyph::Completed:
        p->drawEllipse(QPointF(8.0, 8.0), 5.3, 5.3);
        p->drawPolyline(QPolygonF() << QPointF(5.5, 8.3)
                                    << QPointF(7.3, 10.1)
                                    << QPointF(10.7, 6.1));
        break;

    case Glyph::Failed:
        p->drawPolyline(QPolygonF() << QPointF(8.0, 2.7)               // 三角
                                    << QPointF(13.5, 12.8)
                                    << QPointF(2.5, 12.8)
                                    << QPointF(8.0, 2.7));
        p->drawLine(QPointF(8.0, 6.4), QPointF(8.0, 9.5));             // 感叹号
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawEllipse(QPointF(8.0, 11.3), 0.85, 0.85);
        break;

    case Glyph::Cancelled:
        p->drawEllipse(QPointF(8.0, 8.0), 5.3, 5.3);
        p->drawLine(QPointF(4.25, 4.25), QPointF(11.75, 11.75));
        break;

    case Glyph::Compressed:
        p->drawRoundedRect(QRectF(3.1, 3.1, 9.8, 9.8), 1.4, 1.4);
        p->drawLine(QPointF(3.1, 6.3), QPointF(12.9, 6.3));            // 盒盖
        p->drawLine(QPointF(8.0, 6.3), QPointF(8.0, 12.9));            // 拉链条
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRect(QRectF(7.2, 7.6, 1.6, 1.2));                       // 拉链齿
        p->drawRect(QRectF(7.2, 9.8, 1.6, 1.2));
        break;

    case Glyph::Document:
        p->drawPolyline(QPolygonF() << QPointF(4.1, 2.6)               // 页面 + 折角
                                    << QPointF(9.3, 2.6)
                                    << QPointF(12.3, 5.7)
                                    << QPointF(12.3, 13.4)
                                    << QPointF(4.1, 13.4)
                                    << QPointF(4.1, 2.6));
        p->drawPolyline(QPolygonF() << QPointF(9.3, 2.6)
                                    << QPointF(9.3, 5.7)
                                    << QPointF(12.3, 5.7));
        p->drawLine(QPointF(6.2, 8.3), QPointF(10.2, 8.3));            // 正文线
        p->drawLine(QPointF(6.2, 10.8), QPointF(10.2, 10.8));
        break;

    case Glyph::Music:
        p->setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(QPointF(6.9, 11.1), QPointF(6.9, 4.3));            // 符干
        p->drawLine(QPointF(12.1, 9.7), QPointF(12.1, 2.9));
        p->drawLine(QPointF(6.9, 4.3), QPointF(12.1, 2.9));            // 符梁
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawEllipse(QPointF(5.4, 11.1), 1.6, 1.3);                  // 符头
        p->drawEllipse(QPointF(10.6, 9.7), 1.6, 1.3);
        break;

    case Glyph::Program:
        p->drawRoundedRect(QRectF(2.7, 3.4, 10.6, 9.4), 1.5, 1.5);
        p->drawLine(QPointF(2.7, 6.3), QPointF(13.3, 6.3));            // 标题栏
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawEllipse(QPointF(4.7, 4.85), 0.7, 0.7);                  // 三个圆点
        p->drawEllipse(QPointF(6.9, 4.85), 0.7, 0.7);
        p->drawEllipse(QPointF(9.1, 4.85), 0.7, 0.7);
        break;

    case Glyph::Video:
        p->drawRoundedRect(QRectF(2.7, 3.4, 10.6, 9.4), 1.5, 1.5);
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawPolygon(QPolygonF() << QPointF(6.6, 5.7)
                                   << QPointF(11.0, 8.1)
                                   << QPointF(6.6, 10.5));
        break;

    case Glyph::OtherFile:
        // 光页 + 折角，不画正文线：与 Document 区分开（否则「文档」和「其他」两个图标一样）
        p->drawPolyline(QPolygonF() << QPointF(4.1, 2.6)
                                    << QPointF(9.3, 2.6)
                                    << QPointF(12.3, 5.7)
                                    << QPointF(12.3, 13.4)
                                    << QPointF(4.1, 13.4)
                                    << QPointF(4.1, 2.6));
        p->drawPolyline(QPolygonF() << QPointF(9.3, 2.6)
                                    << QPointF(9.3, 5.7)
                                    << QPointF(12.3, 5.7));
        break;

    case Glyph::More:
        // 三个点：工具栏「更多」溢出菜单。用点而不是省略号字符 ——
        // 字符在不同字体下的基线与间距不可控，小尺寸下会偏。
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawEllipse(QPointF(4.2, 8.0), 1.15, 1.15);
        p->drawEllipse(QPointF(8.0, 8.0), 1.15, 1.15);
        p->drawEllipse(QPointF(11.8, 8.0), 1.15, 1.15);
        break;
    }

    p->restore();
}

// ── 图标引擎 ────────────────────────────────
// 关键点：paint() 里实时取色，所以主题切换后所有控件重绘即变色，
// 不需要在主题切换时重建/替换任何 QIcon（否则极易漏掉分类树、右键菜单等位置）。
namespace {

class GlyphIconEngine : public QIconEngine {
public:
    explicit GlyphIconEngine(Glyph g, Tint t = Tint::Default) : m_glyph(g), m_tint(t) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State /*state*/) override
    {
        painter->save();
        const qreal s = qMin(rect.width(), rect.height()) / 16.0;
        painter->translate(rect.left() + rect.width() / 2.0,
                           rect.top() + rect.height() / 2.0);
        painter->scale(s, s);
        painter->translate(-8.0, -8.0);
        // 语义着色槽：只在「正常/选中」态生效；禁用态一律降对比，
        // 否则一个亮绿/亮玫红的图标会让"这一项不可用"完全看不出来。
        const QColor col = (m_tint != Tint::Default && mode == QIcon::Normal)
                               ? tintColor(m_tint)
                               : colorForMode(mode);
        paintGlyph(painter, m_glyph, col);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
        QPixmap pm(QSize(qMax(1, qRound(size.width() * dpr)),
                         qMax(1, qRound(size.height() * dpr))));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

    QIconEngine* clone() const override { return new GlyphIconEngine(m_glyph, m_tint); }

    // key 随主题、色调与设备像素比变化，避免 Qt 复用旧配色/旧分辨率的缓存位图
    QString key() const override
    {
        return QStringLiteral("idm-glyph-%1-%2-%3-%4")
            .arg(int(m_glyph))
            .arg(int(m_tint))
            .arg(g_dark ? 1 : 0)
            .arg(qApp ? qApp->devicePixelRatio() : 1.0, 0, 'f', 2);
    }

private:
    Glyph m_glyph;
    Tint  m_tint;
};

} // namespace

namespace {
// 图标缓存：QIcon 本身与主题无关（颜色由 engine 在 paint 时解析），
// 所以可以安全地按 (字形, 色调) 复用。没有这层缓存，表格每次重绘都要
// 为每一行 new 一个 QIconEngine —— 任务上千时这是每秒几千次分配。
// 只在 GUI 线程访问，无需加锁。
//
// 刻意用 new 泄漏而不用函数内静态对象：QIcon 内部会缓存 QPixmap，
// 而静态对象在 main() 返回之后才析构 —— 那时 QApplication 已经没了，
// 销毁残留 QPixmap 是 Qt 明确不建议的顺序（可能退出时崩）。条目上限只有
// 字形数×色调数（几十个），泄漏这点内存没有任何代价。
QHash<int, QIcon>& iconCache()
{
    static QHash<int, QIcon>* cache = new QHash<int, QIcon>();
    return *cache;
}

QIcon cachedIcon(Glyph g, Tint t)
{
    QHash<int, QIcon>& cache = iconCache();
    const int key = int(g) * 1000 + int(t);
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();
    const QIcon ic(new GlyphIconEngine(g, t));
    cache.insert(key, ic);
    return ic;
}
} // namespace

QIcon icon(Glyph g)
{
    return cachedIcon(g, Tint::Default);
}

QIcon icon(Glyph g, Tint t)
{
    return cachedIcon(g, t);
}

QIcon fileTypeIcon(const QString& fileName)
{
    switch (CategoryFilterProxy::detectFileType(fileName)) {
        case FileType::Compressed: return cachedIcon(Glyph::Compressed, Tint::Amber);
        case FileType::Document:   return cachedIcon(Glyph::Document,   Tint::Indigo);
        case FileType::Music:      return cachedIcon(Glyph::Music,      Tint::Purple);
        case FileType::Program:    return cachedIcon(Glyph::Program,    Tint::Green);
        case FileType::Video:      return cachedIcon(Glyph::Video,      Tint::Rose);
        default:                   return cachedIcon(Glyph::OtherFile,  Tint::Slate);
    }
}

} // namespace AppIcons
