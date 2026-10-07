# 文档图与复现

这里的图是最终目标设计，不是当前运行结果。`.puml` 为可编辑源文件，同名 `.svg` 为可直接查看的渲染结果；需求和设计文档引用 SVG。

| 图 | 对应内容 |
| --- | --- |
| requirements-use-cases | 12 个用例、参与者与阶段 |
| design-classes | 领域实体与 Agent/Server 核心服务 |
| design-components | 构件职责、本地接口和传输边界 |
| design-deployment-local / remote | 两种部署及数据所在主机 |
| design-manual-backup | 暂存、校验、完整提交与失败 |
| design-realtime-backup | 窗口关闭、事件合并、对账和等待 |
| design-restore | 空目录校验、受限恢复与部分失败 |
| design-ui-flow / pages | 操作流程及四个主要页面线框 |

本次使用 PlantUML 1.2026.8、Java 21、Noto Sans CJK SC 中文字体。静态 UML 图启用 Smetana 布局，不需要 Graphviz；顺序图、活动图与 Salt 线框由 PlantUML 自身布局。生成全部 SVG：

~~~bash
java -Djava.awt.headless=true -jar /path/to/plantuml.jar \
  -charset UTF-8 -tsvg docs/diagrams/*.puml
~~~

在项目根目录执行。字体应先安装到渲染机；查看 SVG 的浏览器也应有中文字体。更改源文件后重新生成同名 SVG，并检查文字、箭头和对应文档编号。不要向外部在线渲染服务提交包含私人路径或账户信息的图。
