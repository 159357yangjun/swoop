#include "schedule_dialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>

namespace {

/* 「自旋框右下 1px 深色接缝」的补丁控件。
 *
 * 现象：开了日历弹窗的 QDateTimeEdit，只要样式表给它自己、或给它的 ::down-button /
 *       ::down-arrow 写了框，字段内右侧就会出现一条贯穿整个内高的 1px 深色竖线，
 *       底边右端另有一段 16px 深色横线，两段拼成直角 L。字段少的时候看着像阴影。
 *
 * 成因：QStyleSheetStyle 在处理「有样式表的自旋框」时，会把自旋按钮区交回 base style
 *       再画一遍；那个凸起浮雕的受光边在左上（浅色底上看不见），暗边落在右下，
 *       于是只剩下这根 L。
 *
 * ⚠️ 不要试图用 QSS 解决，都已实测无效（tools/seam_probe.py 可复现）：
 *     给 ::down-button 绝对定位铺满整列、给不透明底色、指定 height、压成 0x0、
 *     buttonSymbols=NoButtons、qproperty-frame:false、换 base style……L 都原样在。
 *   也不要试图用调色板解决：setPalette 把 QPalette::Dark 改成纯红，实测截图里
 *     红色像素为 0 —— 设了 QSS 的控件不认 widget palette，这条路是死的。
 *
 * 做法：基础绘制完成后，用输入框底色把那 2px 浮雕重新抹一遍。只覆盖紧贴边框内侧的
 *       两列与底部两行：不碰边框本身（边框在更外面 1px），也不碰日历图标
 *       （图标距右缘还有 7px，纵向居中，与底部那两行不重叠）。
 *       16 不是随手写的数，是 base style 里自旋按钮区的固定宽度，也正是底部暗段长度。 */
class SeamFreeDateTimeEdit : public QDateTimeEdit {
public:
    using QDateTimeEdit::QDateTimeEdit;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QDateTimeEdit::paintEvent(event);

        constexpr int kBorder = 1;     // 主题 QSS 里输入框边框宽度
        constexpr int kBevel = 2;      // 实测浮雕厚度：内侧 1px 深(85) + 1px 中灰(170)
        constexpr int kButtonW = 16;   // base style 里自旋按钮区宽度 = 底部暗段长度
        const int w = width();
        const int h = height();
        if (w <= 2 * kBorder + kButtonW + 1 || h <= 2 * kBorder + kBevel) {
            return;                    // 小到摆不下就不画，避免越界
        }
        QPainter p(this);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().brush(QPalette::Base));
        // 右内侧那 2px 竖条
        p.drawRect(w - kBorder - kBevel, kBorder, kBevel, h - 2 * kBorder);
        // 底边右端那 2px 横条（比暗段多留 1px 余量，右端与竖条对齐）
        p.drawRect(w - kButtonW - kBorder - 1, h - kBorder - kBevel, kButtonW + 1, kBevel);
    }
};

}  // namespace

ScheduleDialog::ScheduleDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("定时下载设置"));
    setMinimumWidth(360);

    auto* mainLayout = new QVBoxLayout(this);

    // 启用开关
    m_enableCheck = new QCheckBox(QStringLiteral("启用定时下载"), this);
    m_enableCheck->setChecked(true);
    mainLayout->addWidget(m_enableCheck);

    // 时间设置
    auto* group = new QGroupBox(QStringLiteral("定时设置"), this);
    auto* form = new QFormLayout(group);
    form->setLabelAlignment(Qt::AlignRight);

    m_dateTimeEdit = new SeamFreeDateTimeEdit(this);
    m_dateTimeEdit->setDateTime(QDateTime::currentDateTime().addSecs(3600));  // 默认1小时后
    m_dateTimeEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_dateTimeEdit->setCalendarPopup(true);
    m_dateTimeEdit->setToolTip(QStringLiteral("点击右侧日历图标选择日期时间"));
    /* ⚠️ 这里**不要**去找 QToolButton 挂图标——踩过的坑，记下来免得再犯：
     * 实测（Qt 6.11）QDateTimeEdit 开了日历弹窗后，子控件只有 QLineEdit[qt_spinbox_lineedit]
     * 和 QCalendarPopup，**压根没有 QToolButton**。调过 calendarWidget() 之后再
     * findChild<QToolButton*>() 会命中日历内部的 QtPrivate::QPrevNextCalButton
     * （就是日历左上角「上一月」那个按钮），把图标错挂到它身上，还误以为改成功了。
     * 真正的「打开日历」按钮是样式画的子控件（SC_SpinBoxDown，热区=整条右侧竖列），
     * 样式表管不到 QPainter 之外的东西，只能靠 QSS 管 —— 位置和图标都写在
     * resources/qss/{light,dark}.qss 的 QDateTimeEdit::down-button / ::down-arrow 里。 */
    form->addRow(QStringLiteral("开始时间:"), m_dateTimeEdit);

    m_recurringCheck = new QCheckBox(QStringLiteral("每日重复"), this);
    m_recurringCheck->setChecked(false);
    form->addRow(QStringLiteral("重复:"), m_recurringCheck);

    auto* hint = new QLabel(QStringLiteral(
        "提示：到点后自动开始该任务。勾选「每日重复」则在每天的同一时刻自动开始。\n"
        "定时设置会随配置一起保存，重启程序后仍然有效。\n"
        "程序未运行时不会下载：下次启动时若时间已过，会补跑一次，"
        "重复任务随后对齐到下一个未来时刻。"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    form->addRow(QString(), hint);

    mainLayout->addWidget(group);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 启用开关切换时，禁用/启用时间控件
    connect(m_enableCheck, &QCheckBox::toggled, m_dateTimeEdit, &QWidget::setEnabled);
    connect(m_enableCheck, &QCheckBox::toggled, m_recurringCheck, &QWidget::setEnabled);
}

QDateTime ScheduleDialog::scheduledTime() const
{
    return m_dateTimeEdit->dateTime();
}

bool ScheduleDialog::isEnabled() const
{
    return m_enableCheck->isChecked();
}

bool ScheduleDialog::isRecurring() const
{
    return m_recurringCheck->isChecked();
}

void ScheduleDialog::setScheduledTime(const QDateTime& dt)
{
    m_dateTimeEdit->setDateTime(dt);
}

void ScheduleDialog::setEnabled(bool enabled)
{
    m_enableCheck->setChecked(enabled);
}

void ScheduleDialog::setRecurring(bool recurring)
{
    m_recurringCheck->setChecked(recurring);
}
